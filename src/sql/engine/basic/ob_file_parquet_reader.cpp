/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_file_parquet_reader.h"

#include <zlib.h>
#include <math.h>

#include "lib/oblog/ob_log_module.h"
#include "lib/compress/zstd_1_3_8/ob_zstd_wrapper.h"
#include "share/ob_errno.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
const int64_t MAX_FOOTER_SIZE = 64L * 1024 * 1024;
const int64_t MAX_PAGE_SIZE = 256L * 1024 * 1024;
const int64_t MAX_PAGE_HEADER_SIZE = 16L * 1024 * 1024;
const int64_t MAX_THRIFT_DEPTH = 32;

// thrift compact protocol types
enum ThriftCompactType
{
  TC_STOP = 0,
  TC_TRUE = 1,
  TC_FALSE = 2,
  TC_BYTE = 3,
  TC_I16 = 4,
  TC_I32 = 5,
  TC_I64 = 6,
  TC_DOUBLE = 7,
  TC_BINARY = 8,
  TC_LIST = 9,
  TC_SET = 10,
  TC_MAP = 11,
  TC_STRUCT = 12,
};

// parquet enums
const int32_t PQ_REQUIRED = 0;
const int32_t PQ_REPEATED = 2;
const int32_t PQ_PAGE_DATA = 0;
const int32_t PQ_PAGE_DICTIONARY = 2;
const int32_t PQ_PAGE_DATA_V2 = 3;
const int32_t PQ_ENC_PLAIN = 0;
const int32_t PQ_ENC_PLAIN_DICTIONARY = 2;
const int32_t PQ_ENC_RLE = 3;
const int32_t PQ_ENC_RLE_DICTIONARY = 8;
const int32_t PQ_CODEC_UNCOMPRESSED = 0;
const int32_t PQ_CODEC_SNAPPY = 1;
const int32_t PQ_CODEC_GZIP = 2;
const int32_t PQ_CODEC_ZSTD = 6;
// ConvertedType
const int32_t PQ_CT_DECIMAL = 5;
const int32_t PQ_CT_DATE = 6;
const int32_t PQ_CT_TIME_MILLIS = 7;
const int32_t PQ_CT_TIME_MICROS = 8;
const int32_t PQ_CT_TIMESTAMP_MILLIS = 9;
const int32_t PQ_CT_TIMESTAMP_MICROS = 10;
const int32_t PQ_CT_UINT_8 = 11;
const int32_t PQ_CT_UINT_16 = 12;
const int32_t PQ_CT_UINT_32 = 13;
const int32_t PQ_CT_UINT_64 = 14;
// LogicalType union members
const int32_t PQ_LT_DECIMAL = 5;
const int32_t PQ_LT_DATE = 6;
const int32_t PQ_LT_TIME = 7;
const int32_t PQ_LT_TIMESTAMP = 8;
const int32_t PQ_LT_INTEGER = 10;
const int32_t PQ_LT_FLOAT16 = 15;

const char *codec_name(const int32_t codec)
{
  static const char *names[] = {"UNCOMPRESSED", "SNAPPY", "GZIP", "LZO", "BROTLI", "LZ4", "ZSTD",
                                "LZ4_RAW"};
  return codec >= 0 && codec < static_cast<int32_t>(ARRAYSIZEOF(names)) ? names[codec] : "UNKNOWN";
}

const char *encoding_name(const int32_t encoding)
{
  static const char *names[] = {"PLAIN", "GROUP_VAR_INT", "PLAIN_DICTIONARY", "RLE", "BIT_PACKED",
                                "DELTA_BINARY_PACKED", "DELTA_LENGTH_BYTE_ARRAY",
                                "DELTA_BYTE_ARRAY", "RLE_DICTIONARY", "BYTE_STREAM_SPLIT"};
  return encoding >= 0 && encoding < static_cast<int32_t>(ARRAYSIZEOF(names))
         ? names[encoding] : "UNKNOWN";
}

void *zstd_alloc(void *opaque, size_t size)
{
  return NULL == opaque ? NULL : static_cast<ObIAllocator *>(opaque)->alloc(size);
}

void zstd_free(void *opaque, void *address)
{
  if (NULL != opaque) {
    static_cast<ObIAllocator *>(opaque)->free(address);
  }
}

inline int32_t read_le32(const char *p)
{
  const uint8_t *u = reinterpret_cast<const uint8_t *>(p);
  return static_cast<int32_t>(static_cast<uint32_t>(u[0]) | (static_cast<uint32_t>(u[1]) << 8)
                              | (static_cast<uint32_t>(u[2]) << 16)
                              | (static_cast<uint32_t>(u[3]) << 24));
}

inline int64_t read_le64(const char *p)
{
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) {
    v = (v << 8) | static_cast<uint8_t>(p[i]);
  }
  return static_cast<int64_t>(v);
}

int32_t bit_width_of(const int64_t max_value)
{
  int32_t width = 0;
  while (width < 63 && (1L << width) <= max_value) {
    ++width;
  }
  return width;
}

// Reader of the thrift compact protocol. Running out of input is OB_BUF_NOT_ENOUGH, so a
// page header read through a too small window can be retried with a larger one.
class ThriftReader
{
public:
  ThriftReader(const char *buf, const int64_t len)
    : buf_(reinterpret_cast<const uint8_t *>(buf)), len_(len), pos_(0) {}
  int64_t pos() const { return pos_; }
  int read_byte(uint8_t &v)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_BUF_NOT_ENOUGH;
    } else {
      v = buf_[pos_++];
    }
    return ret;
  }
  int read_varint(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    uint8_t b = 0;
    int shift = 0;
    v = 0;
    do {
      if (shift > 63) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(read_byte(b))) {
      } else {
        v |= static_cast<uint64_t>(b & 0x7f) << shift;
        shift += 7;
      }
    } while (OB_SUCC(ret) && 0 != (b & 0x80));
    return ret;
  }
  int read_int(const uint8_t type, int64_t &v)
  {
    int ret = OB_SUCCESS;
    uint64_t u = 0;
    uint8_t b = 0;
    if (TC_BYTE == type) {
      if (OB_SUCC(read_byte(b))) {
        v = static_cast<int8_t>(b);
      }
    } else if (TC_I16 != type && TC_I32 != type && TC_I64 != type) {
      ret = OB_INVALID_DATA;
    } else if (OB_SUCC(read_varint(u))) {
      v = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
    }
    return ret;
  }
  int read_i32(const uint8_t type, int32_t &v)
  {
    int ret = OB_SUCCESS;
    int64_t w = 0;
    if (OB_FAIL(read_int(type, w))) {
    } else if (w < INT32_MIN || w > INT32_MAX) {
      ret = OB_INVALID_DATA;
    } else {
      v = static_cast<int32_t>(w);
    }
    return ret;
  }
  int read_binary(const uint8_t type, ObString &s)
  {
    int ret = OB_SUCCESS;
    uint64_t n = 0;
    if (TC_BINARY != type) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(read_varint(n))) {
    } else if (n > static_cast<uint64_t>(len_ - pos_)) {
      ret = OB_BUF_NOT_ENOUGH;
    } else if (n > INT32_MAX) {
      ret = OB_INVALID_DATA;
    } else {
      s.assign_ptr(reinterpret_cast<const char *>(buf_ + pos_), static_cast<int32_t>(n));
      pos_ += static_cast<int64_t>(n);
    }
    return ret;
  }
  // type is TC_STOP at the end of a struct
  int read_field(int16_t &last_id, uint8_t &type, int16_t &id)
  {
    int ret = OB_SUCCESS;
    uint8_t h = 0;
    if (OB_FAIL(read_byte(h))) {
    } else {
      type = h & 0x0f;
      id = 0;
      if (TC_STOP != type) {
        const int16_t delta = static_cast<int16_t>(h >> 4);
        if (0 == delta) {
          int64_t v = 0;
          if (OB_FAIL(read_int(TC_I16, v))) {
          } else {
            id = static_cast<int16_t>(v);
          }
        } else {
          id = static_cast<int16_t>(last_id + delta);
        }
        last_id = id;
      }
    }
    return ret;
  }
  int read_list_header(uint8_t &elem_type, int64_t &size)
  {
    int ret = OB_SUCCESS;
    uint8_t h = 0;
    if (OB_FAIL(read_byte(h))) {
    } else {
      elem_type = h & 0x0f;
      size = h >> 4;
      if (15 == size) {
        uint64_t n = 0;
        if (OB_FAIL(read_varint(n))) {
        } else if (n > static_cast<uint64_t>(len_)) {
          ret = OB_INVALID_DATA;
        } else {
          size = static_cast<int64_t>(n);
        }
      }
      // every element takes at least one byte
      if (OB_SUCC(ret) && size > len_ - pos_) {
        ret = OB_BUF_NOT_ENOUGH;
      }
    }
    return ret;
  }
  int skip(const uint8_t type, const int64_t depth)
  {
    int ret = OB_SUCCESS;
    uint64_t u = 0;
    uint8_t b = 0;
    ObString s;
    if (depth > MAX_THRIFT_DEPTH) {
      ret = OB_INVALID_DATA;
    } else {
      switch (type) {
        case TC_TRUE:
        case TC_FALSE:
          break;
        case TC_BYTE:
          ret = read_byte(b);
          break;
        case TC_I16:
        case TC_I32:
        case TC_I64:
          ret = read_varint(u);
          break;
        case TC_DOUBLE:
          if (len_ - pos_ < 8) {
            ret = OB_BUF_NOT_ENOUGH;
          } else {
            pos_ += 8;
          }
          break;
        case TC_BINARY:
          ret = read_binary(type, s);
          break;
        case TC_LIST:
        case TC_SET: {
          uint8_t et = 0;
          int64_t n = 0;
          if (OB_SUCC(read_list_header(et, n))) {
            for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
              ret = skip_element(et, depth + 1);
            }
          }
          break;
        }
        case TC_MAP: {
          uint8_t kv = 0;
          if (OB_FAIL(read_varint(u))) {
          } else if (u > static_cast<uint64_t>(len_)) {
            ret = OB_INVALID_DATA;
          } else if (u > 0 && OB_FAIL(read_byte(kv))) {
          }
          for (uint64_t i = 0; OB_SUCC(ret) && i < u; ++i) {
            if (OB_FAIL(skip_element(kv >> 4, depth + 1))) {
            } else if (OB_FAIL(skip_element(kv & 0x0f, depth + 1))) {
            }
          }
          break;
        }
        case TC_STRUCT:
          ret = skip_struct(depth + 1);
          break;
        default:
          ret = OB_INVALID_DATA;
          break;
      }
    }
    return ret;
  }
  int skip_struct(const int64_t depth)
  {
    int ret = OB_SUCCESS;
    int16_t last = 0;
    uint8_t type = TC_STOP;
    int16_t id = 0;
    if (depth > MAX_THRIFT_DEPTH) {
      ret = OB_INVALID_DATA;
    }
    while (OB_SUCC(ret)) {
      if (OB_FAIL(read_field(last, type, id))) {
      } else if (TC_STOP == type) {
        break;
      } else if (OB_FAIL(skip(type, depth))) {
      }
    }
    return ret;
  }
private:
  // booleans inside containers take one byte
  int skip_element(const uint8_t type, const int64_t depth)
  {
    uint8_t b = 0;
    return TC_TRUE == type || TC_FALSE == type ? read_byte(b) : skip(type, depth);
  }
private:
  const uint8_t *buf_;
  int64_t len_;
  int64_t pos_;
};

struct SchemaElem
{
  SchemaElem() : leaf_(), num_children_(0), has_type_(false) {}
  ParquetLeafMeta leaf_;
  int32_t num_children_;
  bool has_type_;
};

int parse_time_unit(ThriftReader &r, int32_t &unit)
{
  int ret = OB_SUCCESS;
  int16_t last = 0;
  uint8_t type = TC_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(r.read_field(last, type, id))) {
    } else if (TC_STOP == type) {
      break;
    } else {
      unit = id;
      ret = r.skip(type, 1);
    }
  }
  return ret;
}

int parse_logical_type(ThriftReader &r, ParquetLeafMeta &leaf)
{
  int ret = OB_SUCCESS;
  int16_t last = 0;
  uint8_t type = TC_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(r.read_field(last, type, id))) {
    } else if (TC_STOP == type) {
      break;
    } else if (TC_STRUCT != type) {
      ret = r.skip(type, 1);
    } else {
      leaf.logical_kind_ = id;
      int16_t inner_last = 0;
      uint8_t inner_type = TC_STOP;
      int16_t inner_id = 0;
      while (OB_SUCC(ret)) {
        if (OB_FAIL(r.read_field(inner_last, inner_type, inner_id))) {
        } else if (TC_STOP == inner_type) {
          break;
        } else if (PQ_LT_DECIMAL == id && 1 == inner_id) {
          ret = r.read_i32(inner_type, leaf.scale_);
        } else if (PQ_LT_DECIMAL == id && 2 == inner_id) {
          ret = r.read_i32(inner_type, leaf.precision_);
        } else if ((PQ_LT_TIME == id || PQ_LT_TIMESTAMP == id) && 2 == inner_id
                   && TC_STRUCT == inner_type) {
          ret = parse_time_unit(r, leaf.time_unit_);
        } else if (PQ_LT_INTEGER == id && 1 == inner_id) {
          ret = r.read_i32(inner_type, leaf.int_bit_width_);
        } else if (PQ_LT_INTEGER == id && 2 == inner_id) {
          leaf.int_signed_ = TC_TRUE == inner_type;
        } else {
          ret = r.skip(inner_type, 2);
        }
      }
    }
  }
  return ret;
}

int parse_schema_element(ThriftReader &r, SchemaElem &elem)
{
  int ret = OB_SUCCESS;
  int16_t last = 0;
  uint8_t type = TC_STOP;
  int16_t id = 0;
  ParquetLeafMeta &leaf = elem.leaf_;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(r.read_field(last, type, id))) {
    } else if (TC_STOP == type) {
      break;
    } else {
      switch (id) {
        case 1: elem.has_type_ = true; ret = r.read_i32(type, leaf.physical_type_); break;
        case 2: ret = r.read_i32(type, leaf.type_length_); break;
        case 3: ret = r.read_i32(type, leaf.repetition_); break;
        case 4: ret = r.read_binary(type, leaf.name_); break;
        case 5: ret = r.read_i32(type, elem.num_children_); break;
        case 6: ret = r.read_i32(type, leaf.converted_type_); break;
        case 7: ret = r.read_i32(type, leaf.scale_); break;
        case 8: ret = r.read_i32(type, leaf.precision_); break;
        case 10:
          ret = TC_STRUCT == type ? parse_logical_type(r, leaf) : r.skip(type, 1);
          break;
        default: ret = r.skip(type, 1); break;
      }
    }
  }
  return ret;
}

int parse_column_meta(ThriftReader &r, ParquetChunkMeta &chunk)
{
  int ret = OB_SUCCESS;
  int16_t last = 0;
  uint8_t type = TC_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(r.read_field(last, type, id))) {
    } else if (TC_STOP == type) {
      break;
    } else {
      switch (id) {
        case 1: ret = r.read_i32(type, chunk.physical_type_); break;
        case 4: ret = r.read_i32(type, chunk.codec_); break;
        case 5: ret = r.read_int(type, chunk.num_values_); break;
        case 7: ret = r.read_int(type, chunk.total_compressed_size_); break;
        case 9: ret = r.read_int(type, chunk.data_page_offset_); break;
        case 11: ret = r.read_int(type, chunk.dictionary_page_offset_); break;
        default: ret = r.skip(type, 2); break;
      }
    }
  }
  return ret;
}

int parse_column_chunk(ThriftReader &r, ParquetChunkMeta &chunk, bool &has_meta)
{
  int ret = OB_SUCCESS;
  int16_t last = 0;
  uint8_t type = TC_STOP;
  int16_t id = 0;
  ObString file_path;
  has_meta = false;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(r.read_field(last, type, id))) {
    } else if (TC_STOP == type) {
      break;
    } else if (1 == id) {
      if (OB_SUCC(r.read_binary(type, file_path)) && !file_path.empty()) {
        // column chunks stored in other files are not supported
        ret = OB_NOT_SUPPORTED;
      }
    } else if (3 == id && TC_STRUCT == type) {
      has_meta = true;
      ret = parse_column_meta(r, chunk);
    } else {
      ret = r.skip(type, 1);
    }
  }
  return ret;
}

int parse_row_group(ThriftReader &r, ParquetFileMeta &meta, ParquetRowGroupMeta &group)
{
  int ret = OB_SUCCESS;
  int16_t last = 0;
  uint8_t type = TC_STOP;
  int16_t id = 0;
  group.first_chunk_ = meta.chunks_.count();
  while (OB_SUCC(ret)) {
    if (OB_FAIL(r.read_field(last, type, id))) {
    } else if (TC_STOP == type) {
      break;
    } else if (1 == id && TC_LIST == type) {
      uint8_t et = 0;
      int64_t n = 0;
      if (OB_FAIL(r.read_list_header(et, n))) {
      } else if (TC_STRUCT != et) {
        ret = OB_INVALID_DATA;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
        ParquetChunkMeta chunk;
        bool has_meta = false;
        if (OB_FAIL(parse_column_chunk(r, chunk, has_meta))) {
        } else if (!has_meta) {
          ret = OB_INVALID_DATA;
        } else if (OB_FAIL(meta.chunks_.push_back(chunk))) {
        }
      }
      group.chunk_count_ = meta.chunks_.count() - group.first_chunk_;
    } else if (3 == id) {
      ret = r.read_int(type, group.num_rows_);
    } else {
      ret = r.skip(type, 1);
    }
  }
  return ret;
}

int parse_file_meta(ThriftReader &r, ObIArray<SchemaElem> &schema, ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  int16_t last = 0;
  uint8_t type = TC_STOP;
  int16_t id = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(r.read_field(last, type, id))) {
    } else if (TC_STOP == type) {
      break;
    } else if ((2 == id || 4 == id) && TC_LIST == type) {
      uint8_t et = 0;
      int64_t n = 0;
      if (OB_FAIL(r.read_list_header(et, n))) {
      } else if (TC_STRUCT != et) {
        ret = OB_INVALID_DATA;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
        if (2 == id) {
          SchemaElem elem;
          if (OB_FAIL(parse_schema_element(r, elem))) {
          } else if (OB_FAIL(schema.push_back(elem))) {
          }
        } else {
          ParquetRowGroupMeta group;
          if (OB_FAIL(parse_row_group(r, meta, group))) {
          } else if (OB_FAIL(meta.row_groups_.push_back(group))) {
          }
        }
      }
    } else if (3 == id) {
      ret = r.read_int(type, meta.num_rows_);
    } else {
      ret = r.skip(type, 1);
    }
  }
  return ret;
}

int corrupt_file(const ObString &path, const char *what)
{
  return FileSqlUtil::set_user_error(OB_INVALID_DATA, "read_parquet: '%.*s' is not a valid parquet file: %s",
                                     path.length(), path.ptr(), what);
}

int map_leaf_type(const ParquetLeafMeta &leaf, const ObString &path, FileColumnDesc &col)
{
  int ret = OB_SUCCESS;
  ParquetValueKind kind = ParquetValueKind::INVALID;
  FileColumnType type = FileColumnType::NULL_TYPE;
  int16_t scale = 0;
  const int32_t ct = leaf.converted_type_;
  const int32_t lt = leaf.logical_kind_;
  const bool is_decimal = PQ_LT_DECIMAL == lt || PQ_CT_DECIMAL == ct;
  const char *unsupported = NULL;
  switch (leaf.physical_type_) {
    case PQ_BOOLEAN:
      kind = ParquetValueKind::BOOL;
      type = FileColumnType::BOOLEAN;
      break;
    case PQ_INT32:
      if (PQ_LT_DATE == lt || PQ_CT_DATE == ct) {
        kind = ParquetValueKind::DATE;
        type = FileColumnType::DATE;
      } else if (is_decimal) {
        kind = ParquetValueKind::DECIMAL;
        type = FileColumnType::DOUBLE;
      } else if (PQ_LT_TIME == lt || PQ_CT_TIME_MILLIS == ct) {
        unsupported = "TIME";
      } else if ((PQ_LT_INTEGER == lt && !leaf.int_signed_) || PQ_CT_UINT_8 == ct
                 || PQ_CT_UINT_16 == ct || PQ_CT_UINT_32 == ct) {
        kind = ParquetValueKind::UINT32;
        type = FileColumnType::BIGINT;
      } else {
        kind = ParquetValueKind::INT;
        type = FileColumnType::BIGINT;
      }
      break;
    case PQ_INT64:
      if (PQ_LT_TIMESTAMP == lt || PQ_CT_TIMESTAMP_MILLIS == ct || PQ_CT_TIMESTAMP_MICROS == ct) {
        const int32_t unit = PQ_LT_TIMESTAMP == lt ? leaf.time_unit_
                             : (PQ_CT_TIMESTAMP_MILLIS == ct ? 1 : 2);
        kind = 1 == unit ? ParquetValueKind::TS_MILLIS
               : (2 == unit ? ParquetValueKind::TS_MICROS : ParquetValueKind::TS_NANOS);
        type = FileColumnType::DATETIME;
        scale = 1 == unit ? 3 : 6;
      } else if (is_decimal) {
        kind = ParquetValueKind::DECIMAL;
        type = FileColumnType::DOUBLE;
      } else if (PQ_LT_TIME == lt || PQ_CT_TIME_MICROS == ct) {
        unsupported = "TIME";
      } else if ((PQ_LT_INTEGER == lt && !leaf.int_signed_) || PQ_CT_UINT_64 == ct) {
        kind = ParquetValueKind::UINT64;
        type = FileColumnType::BIGINT;
      } else {
        kind = ParquetValueKind::INT;
        type = FileColumnType::BIGINT;
      }
      break;
    case PQ_INT96:
      kind = ParquetValueKind::INT96;
      type = FileColumnType::DATETIME;
      scale = 6;
      break;
    case PQ_FLOAT:
      kind = ParquetValueKind::FLOAT;
      type = FileColumnType::DOUBLE;
      break;
    case PQ_DOUBLE:
      kind = ParquetValueKind::DOUBLE;
      type = FileColumnType::DOUBLE;
      break;
    case PQ_BYTE_ARRAY:
    case PQ_FIXED_LEN_BYTE_ARRAY:
      if (is_decimal) {
        kind = ParquetValueKind::DECIMAL;
        type = FileColumnType::DOUBLE;
      } else if (PQ_LT_FLOAT16 == lt) {
        unsupported = "FLOAT16";
      } else {
        kind = ParquetValueKind::STRING;
        type = FileColumnType::VARCHAR;
      }
      if (PQ_FIXED_LEN_BYTE_ARRAY == leaf.physical_type_ && leaf.type_length_ <= 0) {
        ret = corrupt_file(path, "fixed length column without length");
      }
      break;
    default:
      ret = corrupt_file(path, "unknown physical type");
      break;
  }
  if (OB_FAIL(ret)) {
  } else if (NULL != unsupported) {
    ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
        "read_parquet: %s column '%.*s' is not supported", unsupported, leaf.name_.length(),
        leaf.name_.ptr());
  } else {
    col.set_type(type);
    col.nullable_ = PQ_REQUIRED != leaf.repetition_;
    col.max_length_ = FileColumnType::VARCHAR == type ? FileSqlLimits::MAX_VARCHAR_CHARS : 1;
    col.scale_ = scale;
    col.pq_physical_type_ = leaf.physical_type_;
    col.pq_value_kind_ = static_cast<int32_t>(kind);
    col.pq_scale_ = is_decimal ? leaf.scale_ : 0;
    col.pq_type_length_ = leaf.type_length_;
    col.pq_max_def_level_ = col.nullable_ ? 1 : 0;
  }
  return ret;
}

const int64_t USEC_PER_DAY = 86400L * 1000000L;

bool datetime_in_range(const int64_t usec)
{
  static const int64_t min_usec = FileSqlUtil::days_from_civil(1, 1, 1) * USEC_PER_DAY;
  static const int64_t max_usec = (FileSqlUtil::days_from_civil(9999, 12, 31) + 1) * USEC_PER_DAY - 1;
  return usec >= min_usec && usec <= max_usec;
}

bool date_in_range(const int64_t days)
{
  return days >= FileSqlUtil::days_from_civil(1, 1, 1)
         && days <= FileSqlUtil::days_from_civil(9999, 12, 31);
}

double pow10_of(const int32_t scale)
{
  return pow(10.0, static_cast<double>(scale));
}
} // anonymous namespace

// ---------------------------------------------------------------------------------------
// footer
// ---------------------------------------------------------------------------------------

int ParquetUtil::read_file_meta(LocalFileReader &file, const ObString &path, ObIAllocator &alloc,
                                ParquetFileMeta &meta)
{
  int ret = OB_SUCCESS;
  const int64_t size = file.get_size();
  char tail[8];
  int64_t read_len = 0;
  int32_t footer_len = 0;
  char *footer = NULL;
  ObSEArray<SchemaElem, 16> schema;
  meta.reset();
  if (size < 12) {
    ret = corrupt_file(path, "file is too small");
  } else if (OB_FAIL(file.pread(tail, 8, size - 8, read_len))) {
  } else if (8 != read_len || 0 != MEMCMP(tail + 4, "PAR1", 4)) {
    ret = corrupt_file(path, "missing PAR1 magic");
  } else if (FALSE_IT(footer_len = read_le32(tail))) {
  } else if (footer_len <= 0 || footer_len > size - 12 || footer_len > MAX_FOOTER_SIZE) {
    ret = corrupt_file(path, "invalid footer length");
  } else if (OB_ISNULL(footer = static_cast<char *>(alloc.alloc(footer_len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(file.pread(footer, footer_len, size - 8 - footer_len, read_len))) {
  } else if (read_len != footer_len) {
    ret = corrupt_file(path, "truncated footer");
  } else {
    ThriftReader r(footer, footer_len);
    if (OB_FAIL(parse_file_meta(r, schema, meta))) {
      if (OB_NOT_SUPPORTED == ret) {
        ret = FileSqlUtil::set_user_error(ret, "read_parquet: column chunks in external files are not supported");
      } else {
        ret = corrupt_file(path, "corrupt footer metadata");
      }
    }
  }
  // flat schema: a root followed by primitive leaves
  if (OB_FAIL(ret)) {
  } else if (schema.count() < 2 || schema.at(0).num_children_ != schema.count() - 1) {
    bool nested = false;
    for (int64_t i = 1; i < schema.count(); ++i) {
      nested = nested || schema.at(i).num_children_ > 0;
    }
    if (nested) {
      ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
          "read_parquet: nested columns in '%.*s' are not supported, only flat schemas can be read",
          path.length(), path.ptr());
    } else {
      ret = corrupt_file(path, "invalid schema");
    }
  }
  for (int64_t i = 1; OB_SUCC(ret) && i < schema.count(); ++i) {
    const SchemaElem &elem = schema.at(i);
    if (elem.num_children_ > 0) {
      ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
          "read_parquet: nested column '%.*s' is not supported, only flat schemas can be read",
          elem.leaf_.name_.length(), elem.leaf_.name_.ptr());
    } else if (PQ_REPEATED == elem.leaf_.repetition_) {
      ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
          "read_parquet: repeated column '%.*s' is not supported",
          elem.leaf_.name_.length(), elem.leaf_.name_.ptr());
    } else if (!elem.has_type_) {
      ret = corrupt_file(path, "column without physical type");
    } else if (OB_FAIL(meta.leaves_.push_back(elem.leaf_))) {
    }
  }
  int64_t total_rows = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < meta.row_groups_.count(); ++i) {
    const ParquetRowGroupMeta &group = meta.row_groups_.at(i);
    if (group.chunk_count_ != meta.leaves_.count() || group.num_rows_ < 0) {
      ret = corrupt_file(path, "row group does not match the schema");
    } else {
      total_rows += group.num_rows_;
      for (int64_t j = 0; OB_SUCC(ret) && j < group.chunk_count_; ++j) {
        const ParquetChunkMeta &chunk = meta.chunks_.at(group.first_chunk_ + j);
        if (chunk.physical_type_ != meta.leaves_.at(j).physical_type_
            || chunk.data_page_offset_ < 4 || chunk.data_page_offset_ >= size
            || chunk.num_values_ < 0) {
          ret = corrupt_file(path, "invalid column chunk metadata");
        }
      }
    }
  }
  if (OB_SUCC(ret) && total_rows != meta.num_rows_) {
    ret = corrupt_file(path, "row counts of row groups do not add up");
  }
  // names point into the footer buffer allocated from alloc
  return ret;
}

int infer_parquet_schema(ObIAllocator &alloc, FileTableDesc &desc)
{
  int ret = OB_SUCCESS;
  LocalFileReader file;
  ObArenaAllocator tmp_alloc("FileSqlPqMeta");
  ParquetFileMeta meta;
  if (OB_FAIL(file.open(desc.real_path_, &desc.identity_))) {
  } else if (OB_FAIL(ParquetUtil::read_file_meta(file, desc.path_, tmp_alloc, meta))) {
  } else if (meta.leaves_.count() > FileSqlLimits::MAX_COLUMN_COUNT) {
    ret = FileSqlUtil::set_user_error(OB_SIZE_OVERFLOW, "read_parquet: more than %ld columns",
                                      FileSqlLimits::MAX_COLUMN_COUNT);
  } else if (OB_FAIL(desc.columns_.init(meta.leaves_.count()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < meta.leaves_.count(); ++i) {
    const ParquetLeafMeta &leaf = meta.leaves_.at(i);
    FileColumnDesc col;
    if (OB_FAIL(map_leaf_type(leaf, desc.path_, col))) {
    } else if (OB_FAIL(ob_write_string(alloc, leaf.name_, col.source_name_))) {
    } else {
      col.source_idx_ = static_cast<int32_t>(i);
      if (OB_FAIL(desc.columns_.push_back(col))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(FileSqlUtil::make_unique_names(alloc, desc.columns_))) {
  } else {
    desc.row_count_ = meta.num_rows_;
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// decompression
// ---------------------------------------------------------------------------------------

int ParquetUtil::snappy_uncompress(const char *src, const int64_t src_len, char *dst,
                                   const int64_t dst_cap, int64_t &dst_len)
{
  int ret = OB_SUCCESS;
  const uint8_t *in = reinterpret_cast<const uint8_t *>(src);
  int64_t ip = 0;
  int64_t op = 0;
  uint64_t expect = 0;
  int shift = 0;
  bool more = true;
  while (OB_SUCC(ret) && more) {
    if (ip >= src_len || shift > 35) {
      ret = OB_INVALID_DATA;
    } else {
      const uint8_t b = in[ip++];
      expect |= static_cast<uint64_t>(b & 0x7f) << shift;
      shift += 7;
      more = 0 != (b & 0x80);
    }
  }
  if (OB_SUCC(ret) && expect > static_cast<uint64_t>(dst_cap)) {
    ret = OB_INVALID_DATA;
  }
  while (OB_SUCC(ret) && ip < src_len) {
    const uint8_t tag = in[ip++];
    if (0 == (tag & 3)) {
      // literal
      int64_t len = (tag >> 2) + 1;
      if (len > 60) {
        const int64_t extra = len - 60;
        if (ip + extra > src_len) {
          ret = OB_INVALID_DATA;
        } else {
          len = 0;
          for (int64_t i = 0; i < extra; ++i) {
            len |= static_cast<int64_t>(in[ip + i]) << (8 * i);
          }
          len += 1;
          ip += extra;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (ip + len > src_len || op + len > static_cast<int64_t>(expect)) {
        ret = OB_INVALID_DATA;
      } else {
        MEMCPY(dst + op, in + ip, len);
        ip += len;
        op += len;
      }
    } else {
      // copy
      int64_t len = 0;
      int64_t offset = 0;
      if (1 == (tag & 3)) {
        len = 4 + ((tag >> 2) & 0x7);
        if (ip >= src_len) {
          ret = OB_INVALID_DATA;
        } else {
          offset = (static_cast<int64_t>(tag >> 5) << 8) | in[ip++];
        }
      } else if (2 == (tag & 3)) {
        len = (tag >> 2) + 1;
        if (ip + 2 > src_len) {
          ret = OB_INVALID_DATA;
        } else {
          offset = in[ip] | (static_cast<int64_t>(in[ip + 1]) << 8);
          ip += 2;
        }
      } else {
        len = (tag >> 2) + 1;
        if (ip + 4 > src_len) {
          ret = OB_INVALID_DATA;
        } else {
          offset = static_cast<uint32_t>(read_le32(src + ip));
          ip += 4;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (offset <= 0 || offset > op || op + len > static_cast<int64_t>(expect)) {
        ret = OB_INVALID_DATA;
      } else {
        // the source may overlap the destination
        for (int64_t i = 0; i < len; ++i) {
          dst[op + i] = dst[op - offset + i];
        }
        op += len;
      }
    }
  }
  if (OB_SUCC(ret) && op != static_cast<int64_t>(expect)) {
    ret = OB_INVALID_DATA;
  }
  if (OB_SUCC(ret)) {
    dst_len = op;
  }
  return ret;
}

int ParquetUtil::gzip_uncompress(const char *src, const int64_t src_len, char *dst,
                                 const int64_t dst_cap, int64_t &dst_len)
{
  int ret = OB_SUCCESS;
  z_stream strm;
  MEMSET(&strm, 0, sizeof(strm));
  if (src_len > UINT32_MAX || dst_cap > UINT32_MAX) {
    ret = OB_INVALID_DATA;
  } else if (Z_OK != inflateInit2(&strm, 15 + 32)) {
    ret = OB_ERR_COMPRESS_DECOMPRESS_DATA;
  } else {
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src));
    strm.avail_in = static_cast<uInt>(src_len);
    strm.next_out = reinterpret_cast<Bytef *>(dst);
    strm.avail_out = static_cast<uInt>(dst_cap);
    if (Z_STREAM_END != inflate(&strm, Z_FINISH)) {
      ret = OB_INVALID_DATA;
    } else {
      dst_len = static_cast<int64_t>(strm.total_out);
    }
    inflateEnd(&strm);
  }
  return ret;
}

int ParquetUtil::zstd_uncompress(const char *src, const int64_t src_len, char *dst,
                                 const int64_t dst_cap, int64_t &dst_len)
{
  using common::zstd_1_3_8::ObZstdWrapper;
  using common::zstd_1_3_8::OB_ZSTD_customMem;
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("FileSqlZstd");
  OB_ZSTD_customMem zstd_mem = {zstd_alloc, zstd_free, &alloc};
  void *ctx = NULL;
  dst_len = 0;
  if (src_len <= 0 || dst_cap <= 0) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(ObZstdWrapper::create_stream_dctx(zstd_mem, ctx))) {
    LOG_WARN("failed to create zstd context", K(ret));
  } else {
    // the stream api reads standard frames; a page may hold several of them
    int64_t consumed_len = 0;
    while (OB_SUCC(ret) && consumed_len < src_len) {
      size_t consumed = 0;
      size_t produced = 0;
      if (dst_len >= dst_cap) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(ObZstdWrapper::decompress_stream(ctx, src + consumed_len,
                                                          src_len - consumed_len, consumed,
                                                          dst + dst_len, dst_cap - dst_len,
                                                          produced))) {
        ret = OB_INVALID_DATA;
      } else if (0 == consumed && 0 == produced) {
        ret = OB_INVALID_DATA;
      } else {
        consumed_len += static_cast<int64_t>(consumed);
        dst_len += static_cast<int64_t>(produced);
      }
    }
    ObZstdWrapper::free_stream_dctx(ctx);
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// RLE / bit packed hybrid
// ---------------------------------------------------------------------------------------

int ParquetRleDecoder::next_run()
{
  int ret = OB_SUCCESS;
  uint64_t header = 0;
  int shift = 0;
  bool more = true;
  while (OB_SUCC(ret) && more) {
    if (pos_ >= len_ || shift > 35) {
      ret = OB_INVALID_DATA;
    } else {
      const uint8_t b = data_[pos_++];
      header |= static_cast<uint64_t>(b & 0x7f) << shift;
      shift += 7;
      more = 0 != (b & 0x80);
    }
  }
  if (OB_FAIL(ret)) {
  } else if (0 != (header & 1)) {
    // bit packed groups of 8 values; a short final run is accepted
    const int64_t groups = static_cast<int64_t>(header >> 1);
    const int64_t bytes = groups * bit_width_;
    bp_left_ = groups * 8;
    bp_bit_pos_ = pos_ * 8;
    if (bit_width_ > 0 && pos_ + bytes > len_) {
      bp_left_ = (len_ - pos_) * 8 / bit_width_;
    }
    pos_ = std::min(len_, pos_ + bytes);
  } else {
    const int64_t nbytes = (bit_width_ + 7) / 8;
    rle_left_ = static_cast<int64_t>(header >> 1);
    if (pos_ + nbytes > len_) {
      ret = OB_INVALID_DATA;
    } else {
      rle_value_ = 0;
      for (int64_t i = 0; i < nbytes; ++i) {
        rle_value_ |= static_cast<uint32_t>(data_[pos_ + i]) << (8 * i);
      }
      pos_ += nbytes;
    }
  }
  return ret;
}

int ParquetRleDecoder::next(uint32_t &value)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && 0 == rle_left_ && 0 == bp_left_) {
    ret = next_run();
  }
  if (OB_FAIL(ret)) {
  } else if (rle_left_ > 0) {
    value = rle_value_;
    --rle_left_;
  } else {
    uint32_t v = 0;
    for (int32_t b = 0; b < bit_width_;) {
      const int64_t byte_idx = bp_bit_pos_ >> 3;
      const int32_t bit_off = static_cast<int32_t>(bp_bit_pos_ & 7);
      const int32_t take = std::min(8 - bit_off, bit_width_ - b);
      const uint32_t bits = (static_cast<uint32_t>(data_[byte_idx]) >> bit_off) & ((1u << take) - 1);
      v |= bits << b;
      b += take;
      bp_bit_pos_ += take;
    }
    value = v;
    --bp_left_;
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// column reader
// ---------------------------------------------------------------------------------------

ParquetColumnReader::ParquetColumnReader()
  : alloc_("FileSqlPq"), file_(NULL), path_(), column_(NULL), chunk_(), next_page_offset_(0),
    chunk_end_(0), values_read_(0), read_buf_(NULL), read_buf_cap_(0), page_buf_(NULL),
    page_buf_cap_(0), page_values_(0), page_value_idx_(0), has_def_levels_(false),
    def_decoder_(), dict_encoded_(false), rle_bool_(false), index_decoder_(), value_ptr_(NULL),
    value_end_(NULL), bool_bit_idx_(0), dict_(NULL), dict_count_(0)
{}

void ParquetColumnReader::reset()
{
  file_ = NULL;
  column_ = NULL;
  read_buf_ = NULL;
  read_buf_cap_ = 0;
  page_buf_ = NULL;
  page_buf_cap_ = 0;
  page_values_ = 0;
  page_value_idx_ = 0;
  values_read_ = 0;
  dict_ = NULL;
  dict_count_ = 0;
  dict_encoded_ = false;
  rle_bool_ = false;
  has_def_levels_ = false;
  alloc_.reset();
}

int ParquetColumnReader::corrupt(const char *what) const
{
  const ObString name = NULL == column_ ? ObString() : column_->source_name_;
  return FileSqlUtil::set_user_error(OB_INVALID_DATA,
      "read_parquet: corrupt column '%.*s' in '%.*s': %s", name.length(), name.ptr(),
      path_.length(), path_.ptr(), what);
}

int ParquetColumnReader::ensure_buf(char *&buf, int64_t &cap, const int64_t need)
{
  int ret = OB_SUCCESS;
  if (need > MAX_PAGE_SIZE) {
    ret = corrupt("page is too large");
  } else if (need > cap) {
    const int64_t new_cap = std::max<int64_t>(std::max<int64_t>(need, cap * 2), 64 * 1024);
    char *new_buf = static_cast<char *>(alloc_.alloc(new_cap));
    if (OB_ISNULL(new_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      buf = new_buf;
      cap = new_cap;
    }
  }
  return ret;
}

int ParquetColumnReader::init(LocalFileReader &file, const ObString &path,
                              const FileColumnDesc &column, const ParquetChunkMeta &chunk)
{
  int ret = OB_SUCCESS;
  reset();
  file_ = &file;
  path_ = path;
  column_ = &column;
  chunk_ = chunk;
  int64_t start = chunk.data_page_offset_;
  if (chunk.dictionary_page_offset_ > 0 && chunk.dictionary_page_offset_ < start) {
    start = chunk.dictionary_page_offset_;
  }
  if (chunk.physical_type_ != column.pq_physical_type_) {
    ret = OB_TABLE_DEFINITION_CHANGED;
    FileSqlUtil::set_user_error(ret, "file sql: '%.*s' changed after its schema was inferred, please retry",
                                path.length(), path.ptr());
  } else if (start < 4 || start >= file.get_size()) {
    ret = corrupt("invalid page offset");
  } else {
    next_page_offset_ = start;
    chunk_end_ = file.get_size() - 8;
  }
  return ret;
}

int ParquetColumnReader::read_bytes(const int64_t offset, const int64_t len, char *&buf)
{
  int ret = OB_SUCCESS;
  int64_t read_len = 0;
  if (offset < 0 || len < 0 || offset + len > chunk_end_) {
    ret = corrupt("page extends beyond the column data");
  } else if (OB_FAIL(ensure_buf(read_buf_, read_buf_cap_, std::max<int64_t>(len, 1)))) {
  } else if (OB_FAIL(file_->pread(read_buf_, len, offset, read_len))) {
  } else if (read_len != len) {
    ret = corrupt("truncated page");
  } else {
    buf = read_buf_;
  }
  return ret;
}

int ParquetColumnReader::uncompress(const char *src, const int64_t src_len, const int64_t dst_len,
                                    char *&dst)
{
  int ret = OB_SUCCESS;
  int64_t out_len = 0;
  if (dst_len < 0) {
    ret = corrupt("invalid page size");
  } else if (PQ_CODEC_UNCOMPRESSED == chunk_.codec_) {
    if (src_len != dst_len) {
      ret = corrupt("page sizes do not match");
    } else {
      dst = const_cast<char *>(src);
    }
  } else if (OB_FAIL(ensure_buf(page_buf_, page_buf_cap_, std::max<int64_t>(dst_len, 1)))) {
  } else {
    if (PQ_CODEC_SNAPPY == chunk_.codec_) {
      ret = ParquetUtil::snappy_uncompress(src, src_len, page_buf_, dst_len, out_len);
    } else if (PQ_CODEC_GZIP == chunk_.codec_) {
      ret = ParquetUtil::gzip_uncompress(src, src_len, page_buf_, dst_len, out_len);
    } else if (PQ_CODEC_ZSTD == chunk_.codec_) {
      ret = ParquetUtil::zstd_uncompress(src, src_len, page_buf_, page_buf_cap_, out_len);
    } else {
      ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
          "read_parquet: %s compression of column '%.*s' is not supported", codec_name(chunk_.codec_),
          column_->source_name_.length(), column_->source_name_.ptr());
    }
    if (OB_NOT_SUPPORTED == ret || OB_ALLOCATE_MEMORY_FAILED == ret) {
    } else if (OB_FAIL(ret) || out_len != dst_len) {
      ret = corrupt("cannot decompress page");
    } else {
      dst = page_buf_;
    }
  }
  return ret;
}

int ParquetColumnReader::read_page_header(const int64_t offset, int32_t &page_type,
                                          int32_t &uncompressed_size, int32_t &compressed_size,
                                          int32_t &num_values, int32_t &encoding,
                                          int32_t &def_encoding, int32_t &def_len,
                                          int32_t &rep_len, bool &is_compressed,
                                          int64_t &header_len)
{
  int ret = OB_SUCCESS;
  int64_t window = 1024;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    const int64_t avail = std::min(window, chunk_end_ - offset);
    char *buf = NULL;
    page_type = -1;
    uncompressed_size = -1;
    compressed_size = -1;
    num_values = -1;
    encoding = -1;
    def_encoding = PQ_ENC_RLE;
    def_len = 0;
    rep_len = 0;
    is_compressed = true;
    if (avail <= 0) {
      ret = corrupt("missing page");
    } else if (OB_FAIL(read_bytes(offset, avail, buf))) {
    } else {
      ThriftReader r(buf, avail);
      int16_t last = 0;
      uint8_t type = TC_STOP;
      int16_t id = 0;
      while (OB_SUCC(ret)) {
        if (OB_FAIL(r.read_field(last, type, id))) {
        } else if (TC_STOP == type) {
          break;
        } else if (1 == id) {
          ret = r.read_i32(type, page_type);
        } else if (2 == id) {
          ret = r.read_i32(type, uncompressed_size);
        } else if (3 == id) {
          ret = r.read_i32(type, compressed_size);
        } else if ((5 == id || 7 == id || 8 == id) && TC_STRUCT == type) {
          // DataPageHeader / DictionaryPageHeader / DataPageHeaderV2
          int16_t inner_last = 0;
          uint8_t inner_type = TC_STOP;
          int16_t inner_id = 0;
          while (OB_SUCC(ret)) {
            if (OB_FAIL(r.read_field(inner_last, inner_type, inner_id))) {
            } else if (TC_STOP == inner_type) {
              break;
            } else if (1 == inner_id) {
              ret = r.read_i32(inner_type, num_values);
            } else if (5 == id && 2 == inner_id) {
              ret = r.read_i32(inner_type, encoding);
            } else if (5 == id && 3 == inner_id) {
              ret = r.read_i32(inner_type, def_encoding);
            } else if (7 == id && 2 == inner_id) {
              ret = r.read_i32(inner_type, encoding);
            } else if (8 == id && 4 == inner_id) {
              ret = r.read_i32(inner_type, encoding);
            } else if (8 == id && 5 == inner_id) {
              ret = r.read_i32(inner_type, def_len);
            } else if (8 == id && 6 == inner_id) {
              ret = r.read_i32(inner_type, rep_len);
            } else if (8 == id && 7 == inner_id) {
              is_compressed = TC_TRUE == inner_type;
            } else {
              ret = r.skip(inner_type, 2);
            }
          }
        } else {
          ret = r.skip(type, 1);
        }
      }
      if (OB_SUCC(ret)) {
        done = true;
        header_len = r.pos();
      } else if (OB_BUF_NOT_ENOUGH == ret && avail < chunk_end_ - offset
                 && window < MAX_PAGE_HEADER_SIZE) {
        ret = OB_SUCCESS;
        window *= 4;
      } else {
        ret = corrupt("invalid page header");
      }
    }
  }
  if (OB_SUCC(ret) && (compressed_size < 0 || uncompressed_size < 0 || def_len < 0 || rep_len < 0)) {
    ret = corrupt("invalid page header");
  }
  return ret;
}

int ParquetColumnReader::load_dictionary(const char *data, const int64_t len, const int32_t num_values)
{
  int ret = OB_SUCCESS;
  char *copy = NULL;
  if (num_values < 0 || PQ_BOOLEAN == column_->pq_physical_type_) {
    ret = corrupt("invalid dictionary page");
  } else if (OB_ISNULL(copy = static_cast<char *>(alloc_.alloc(std::max<int64_t>(len, 1))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(dict_ = static_cast<FileValue *>(
                           alloc_.alloc(sizeof(FileValue) * std::max(num_values, 1))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(copy, data, len);
    const char *ptr = copy;
    const char *end = copy + len;
    dict_count_ = 0;
    for (int32_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
      new (&dict_[i]) FileValue();
      if (OB_FAIL(decode_plain(ptr, end, dict_[i]))) {
      } else {
        ++dict_count_;
      }
    }
  }
  return ret;
}

int ParquetColumnReader::load_page()
{
  int ret = OB_SUCCESS;
  bool loaded = false;
  while (OB_SUCC(ret) && !loaded) {
    int32_t page_type = -1;
    int32_t uncompressed_size = 0;
    int32_t compressed_size = 0;
    int32_t num_values = 0;
    int32_t encoding = -1;
    int32_t def_encoding = PQ_ENC_RLE;
    int32_t def_len = 0;
    int32_t rep_len = 0;
    bool is_compressed = true;
    int64_t header_len = 0;
    char *raw = NULL;
    char *page = NULL;
    const int64_t page_offset = next_page_offset_;
    if (values_read_ >= chunk_.num_values_) {
      ret = corrupt("fewer values than rows");
    } else if (OB_FAIL(read_page_header(page_offset, page_type, uncompressed_size, compressed_size,
                                        num_values, encoding, def_encoding, def_len, rep_len,
                                        is_compressed, header_len))) {
    } else if (FALSE_IT(next_page_offset_ = page_offset + header_len + compressed_size)) {
    } else if (PQ_PAGE_DICTIONARY == page_type) {
      if (PQ_ENC_PLAIN != encoding && PQ_ENC_PLAIN_DICTIONARY != encoding) {
        ret = corrupt("unexpected dictionary encoding");
      } else if (OB_FAIL(read_bytes(page_offset + header_len, compressed_size, raw))) {
      } else if (OB_FAIL(uncompress(raw, compressed_size, uncompressed_size, page))) {
      } else if (OB_FAIL(load_dictionary(page, uncompressed_size, num_values))) {
      }
    } else if (PQ_PAGE_DATA == page_type || PQ_PAGE_DATA_V2 == page_type) {
      const char *values = NULL;
      int64_t values_len = 0;
      const char *levels = NULL;
      int64_t levels_len = 0;
      if (num_values < 0) {
        ret = corrupt("invalid value count");
      } else if (OB_FAIL(read_bytes(page_offset + header_len, compressed_size, raw))) {
      } else if (PQ_PAGE_DATA == page_type) {
        if (OB_FAIL(uncompress(raw, compressed_size, uncompressed_size, page))) {
        } else if (column_->pq_max_def_level_ > 0) {
          if (PQ_ENC_RLE != def_encoding) {
            ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
                "read_parquet: %s definition levels of column '%.*s' are not supported",
                encoding_name(def_encoding), column_->source_name_.length(),
                column_->source_name_.ptr());
          } else if (uncompressed_size < 4) {
            ret = corrupt("missing definition levels");
          } else {
            levels_len = read_le32(page);
            levels = page + 4;
            if (levels_len < 0 || levels_len > uncompressed_size - 4) {
              ret = corrupt("invalid definition levels");
            } else {
              values = levels + levels_len;
              values_len = uncompressed_size - 4 - levels_len;
            }
          }
        } else {
          values = page;
          values_len = uncompressed_size;
        }
      } else {
        // v2: levels are stored uncompressed before the values
        const int64_t level_bytes = static_cast<int64_t>(rep_len) + def_len;
        if (level_bytes > compressed_size || level_bytes > uncompressed_size) {
          ret = corrupt("invalid level sizes");
        } else {
          levels = raw + rep_len;
          levels_len = def_len;
          if (!is_compressed || PQ_CODEC_UNCOMPRESSED == chunk_.codec_) {
            values = raw + level_bytes;
            values_len = compressed_size - level_bytes;
          } else if (OB_FAIL(uncompress(raw + level_bytes, compressed_size - level_bytes,
                                        uncompressed_size - level_bytes, page))) {
          } else {
            values = page;
            values_len = uncompressed_size - level_bytes;
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else {
        has_def_levels_ = column_->pq_max_def_level_ > 0;
        if (has_def_levels_) {
          def_decoder_.init(reinterpret_cast<const uint8_t *>(levels), levels_len,
                            bit_width_of(column_->pq_max_def_level_));
        }
        dict_encoded_ = false;
        rle_bool_ = false;
        if (PQ_ENC_PLAIN == encoding) {
          value_ptr_ = values;
          value_end_ = values + values_len;
          bool_bit_idx_ = 0;
        } else if (PQ_ENC_PLAIN_DICTIONARY == encoding || PQ_ENC_RLE_DICTIONARY == encoding) {
          if (NULL == dict_) {
            ret = corrupt("dictionary page is missing");
          } else if (values_len < 1 || static_cast<uint8_t>(values[0]) > 32) {
            ret = corrupt("invalid dictionary indexes");
          } else {
            dict_encoded_ = true;
            index_decoder_.init(reinterpret_cast<const uint8_t *>(values + 1), values_len - 1,
                                static_cast<uint8_t>(values[0]));
          }
        } else if (PQ_ENC_RLE == encoding && PQ_BOOLEAN == column_->pq_physical_type_) {
          // run length encoded booleans always carry a 4 byte length
          const int64_t rle_len = values_len < 4 ? -1 : read_le32(values);
          if (rle_len < 0 || rle_len > values_len - 4) {
            ret = corrupt("invalid boolean values");
          } else {
            rle_bool_ = true;
            index_decoder_.init(reinterpret_cast<const uint8_t *>(values + 4), rle_len, 1);
          }
        } else {
          ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
              "read_parquet: %s encoding of column '%.*s' is not supported",
              encoding_name(encoding), column_->source_name_.length(),
              column_->source_name_.ptr());
        }
        if (OB_SUCC(ret)) {
          page_values_ = num_values;
          page_value_idx_ = 0;
          loaded = num_values > 0;
        }
      }
    } else {
      // index pages and unknown pages are skipped
    }
  }
  return ret;
}

int ParquetColumnReader::next(FileValue &value)
{
  int ret = OB_SUCCESS;
  uint32_t def = static_cast<uint32_t>(column_->pq_max_def_level_);
  value.reset();
  while (OB_SUCC(ret) && page_value_idx_ >= page_values_) {
    ret = load_page();
  }
  if (OB_FAIL(ret)) {
  } else if (has_def_levels_ && OB_FAIL(def_decoder_.next(def))) {
    ret = corrupt("invalid definition levels");
  } else {
    ++page_value_idx_;
    ++values_read_;
    if (def < static_cast<uint32_t>(column_->pq_max_def_level_)) {
      // NULL
    } else if (dict_encoded_) {
      uint32_t idx = 0;
      if (OB_FAIL(index_decoder_.next(idx))) {
        ret = corrupt("invalid dictionary indexes");
      } else if (idx >= dict_count_) {
        ret = corrupt("dictionary index out of range");
      } else {
        value = dict_[idx];
      }
    } else if (rle_bool_) {
      uint32_t bit = 0;
      if (OB_FAIL(index_decoder_.next(bit))) {
        ret = corrupt("invalid boolean values");
      } else {
        value.is_null_ = false;
        value.int_ = bit & 1;
      }
    } else if (PQ_BOOLEAN == column_->pq_physical_type_) {
      const int64_t byte_idx = bool_bit_idx_ >> 3;
      if (value_ptr_ + byte_idx >= value_end_) {
        ret = corrupt("not enough values");
      } else {
        value.is_null_ = false;
        value.int_ = (static_cast<uint8_t>(value_ptr_[byte_idx]) >> (bool_bit_idx_ & 7)) & 1;
        ++bool_bit_idx_;
      }
    } else if (OB_FAIL(decode_plain(value_ptr_, value_end_, value))) {
    }
  }
  return ret;
}

int ParquetColumnReader::decode_plain(const char *&ptr, const char *end, FileValue &value)
{
  int ret = OB_SUCCESS;
  const int64_t left = end - ptr;
  switch (column_->pq_physical_type_) {
    case PQ_INT32:
      if (left < 4) {
        ret = corrupt("not enough values");
      } else {
        ret = convert_int(read_le32(ptr), value);
        ptr += 4;
      }
      break;
    case PQ_INT64:
      if (left < 8) {
        ret = corrupt("not enough values");
      } else {
        ret = convert_int(read_le64(ptr), value);
        ptr += 8;
      }
      break;
    case PQ_INT96:
      if (left < 12) {
        ret = corrupt("not enough values");
      } else {
        // nanoseconds of the day followed by the julian day
        const int64_t nanos = read_le64(ptr);
        const int64_t julian = read_le32(ptr + 8);
        const int64_t usec = (julian - 2440588) * USEC_PER_DAY + nanos / 1000;
        ptr += 12;
        if (!datetime_in_range(usec)) {
          ret = FileSqlUtil::set_user_error(OB_DATA_OUT_OF_RANGE,
              "read_parquet: timestamp of column '%.*s' is out of the DATETIME range",
              column_->source_name_.length(), column_->source_name_.ptr());
        } else {
          value.is_null_ = false;
          value.int_ = usec;
        }
      }
      break;
    case PQ_FLOAT:
      if (left < 4) {
        ret = corrupt("not enough values");
      } else {
        float f = 0;
        MEMCPY(&f, ptr, 4);
        value.is_null_ = false;
        value.double_ = f;
        ptr += 4;
      }
      break;
    case PQ_DOUBLE:
      if (left < 8) {
        ret = corrupt("not enough values");
      } else {
        MEMCPY(&value.double_, ptr, 8);
        value.is_null_ = false;
        ptr += 8;
      }
      break;
    case PQ_BYTE_ARRAY:
      if (left < 4) {
        ret = corrupt("not enough values");
      } else {
        const int64_t len = static_cast<uint32_t>(read_le32(ptr));
        if (len > left - 4) {
          ret = corrupt("not enough values");
        } else {
          ret = convert_bytes(ptr + 4, len, value);
          ptr += 4 + len;
        }
      }
      break;
    case PQ_FIXED_LEN_BYTE_ARRAY:
      if (left < column_->pq_type_length_) {
        ret = corrupt("not enough values");
      } else {
        ret = convert_bytes(ptr, column_->pq_type_length_, value);
        ptr += column_->pq_type_length_;
      }
      break;
    default:
      ret = corrupt("unexpected physical type");
      break;
  }
  return ret;
}

int ParquetColumnReader::convert_int(const int64_t v, FileValue &value)
{
  int ret = OB_SUCCESS;
  const ParquetValueKind kind = static_cast<ParquetValueKind>(column_->pq_value_kind_);
  const ObString &name = column_->source_name_;
  int64_t usec = 0;
  bool is_datetime = false;
  value.is_null_ = false;
  switch (kind) {
    case ParquetValueKind::INT:
      value.int_ = v;
      break;
    case ParquetValueKind::UINT32:
      value.int_ = static_cast<uint32_t>(v);
      break;
    case ParquetValueKind::UINT64:
      if (v < 0) {
        ret = FileSqlUtil::set_user_error(OB_DATA_OUT_OF_RANGE,
            "read_parquet: unsigned value of column '%.*s' is out of the BIGINT range",
            name.length(), name.ptr());
      } else {
        value.int_ = v;
      }
      break;
    case ParquetValueKind::DATE:
      if (!date_in_range(v)) {
        ret = FileSqlUtil::set_user_error(OB_DATA_OUT_OF_RANGE,
            "read_parquet: date of column '%.*s' is out of the DATE range", name.length(), name.ptr());
      } else {
        value.int_ = v;
      }
      break;
    case ParquetValueKind::TS_MILLIS:
      is_datetime = true;
      if (v > INT64_MAX / 1000 || v < INT64_MIN / 1000) {
        usec = INT64_MIN;
      } else {
        usec = v * 1000;
      }
      break;
    case ParquetValueKind::TS_MICROS:
      is_datetime = true;
      usec = v;
      break;
    case ParquetValueKind::TS_NANOS:
      is_datetime = true;
      usec = v >= 0 ? v / 1000 : -((-v + 999) / 1000);
      break;
    case ParquetValueKind::DECIMAL:
      value.double_ = static_cast<double>(v) / pow10_of(column_->pq_scale_);
      break;
    default:
      ret = corrupt("unexpected integer value");
      break;
  }
  if (OB_SUCC(ret) && is_datetime) {
    if (!datetime_in_range(usec)) {
      ret = FileSqlUtil::set_user_error(OB_DATA_OUT_OF_RANGE,
          "read_parquet: timestamp of column '%.*s' is out of the DATETIME range",
          name.length(), name.ptr());
    } else {
      value.int_ = usec;
    }
  }
  return ret;
}

int ParquetColumnReader::convert_bytes(const char *ptr, const int64_t len, FileValue &value)
{
  int ret = OB_SUCCESS;
  const ParquetValueKind kind = static_cast<ParquetValueKind>(column_->pq_value_kind_);
  const ObString &name = column_->source_name_;
  value.is_null_ = false;
  if (ParquetValueKind::STRING == kind) {
    if (!FileSqlUtil::is_valid_utf8(ptr, len)) {
      ret = FileSqlUtil::set_user_error(OB_ERR_INVALID_CHARACTER_STRING,
          "read_parquet: column '%.*s' of '%.*s' contains a value that is not a valid utf8mb4 string",
          name.length(), name.ptr(), path_.length(), path_.ptr());
    } else if (FileSqlUtil::utf8_char_count(ptr, len) > FileSqlLimits::MAX_VARCHAR_CHARS) {
      ret = FileSqlUtil::set_user_error(OB_ERR_DATA_TOO_LONG,
          "read_parquet: value of column '%.*s' is longer than %ld characters", name.length(),
          name.ptr(), FileSqlLimits::MAX_VARCHAR_CHARS);
    } else {
      value.str_.assign_ptr(ptr, static_cast<int32_t>(len));
    }
  } else if (ParquetValueKind::DECIMAL == kind) {
    // big endian two's complement unscaled value
    if (len <= 0 || len > 16) {
      ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
          "read_parquet: decimal column '%.*s' wider than 16 bytes is not supported",
          name.length(), name.ptr());
    } else {
      __int128 v = static_cast<int8_t>(ptr[0]);
      for (int64_t i = 1; i < len; ++i) {
        v = v * 256 + static_cast<uint8_t>(ptr[i]);
      }
      value.double_ = static_cast<double>(v) / pow10_of(column_->pq_scale_);
    }
  } else {
    ret = corrupt("unexpected binary value");
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// row source
// ---------------------------------------------------------------------------------------

ParquetRowSource::ParquetRowSource()
  : meta_alloc_("FileSqlPqMeta"), desc_(NULL), file_(), meta_(), column_idxs_(), readers_(NULL),
    reader_count_(0), group_idx_(-1), rows_left_(0)
{}

int ParquetRowSource::open(const FileTableDesc &desc, const ObIArray<int64_t> &column_idxs,
                           const ObString &secure_file_priv)
{
  int ret = OB_SUCCESS;
  UNUSED(secure_file_priv);
  close();
  desc_ = &desc;
  if (OB_FAIL(column_idxs_.assign(column_idxs))) {
  } else if (OB_FAIL(file_.open(desc.real_path_, &desc.identity_))) {
  } else if (OB_FAIL(ParquetUtil::read_file_meta(file_, desc.path_, meta_alloc_, meta_))) {
  } else if (meta_.leaves_.count() != desc.get_column_count()) {
    ret = OB_TABLE_DEFINITION_CHANGED;
    FileSqlUtil::set_user_error(ret, "file sql: '%.*s' changed after its schema was inferred, please retry",
                                desc.path_.length(), desc.path_.ptr());
  }
  for (int64_t k = 0; OB_SUCC(ret) && k < column_idxs.count(); ++k) {
    if (column_idxs.at(k) < 0 || column_idxs.at(k) >= desc.get_column_count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid column index", K(ret), K(k), K(column_idxs.at(k)));
    }
  }
  if (OB_SUCC(ret) && column_idxs.count() > 0) {
    void *buf = meta_alloc_.alloc(sizeof(ParquetColumnReader) * column_idxs.count());
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      readers_ = static_cast<ParquetColumnReader *>(buf);
      for (int64_t k = 0; k < column_idxs.count(); ++k) {
        new (&readers_[k]) ParquetColumnReader();
      }
      reader_count_ = column_idxs.count();
    }
  }
  group_idx_ = -1;
  rows_left_ = 0;
  return ret;
}

int ParquetRowSource::open_row_group(const int64_t group_idx)
{
  int ret = OB_SUCCESS;
  const ParquetRowGroupMeta &group = meta_.row_groups_.at(group_idx);
  for (int64_t k = 0; OB_SUCC(ret) && k < reader_count_; ++k) {
    const FileColumnDesc &col = desc_->columns_.at(column_idxs_.at(k));
    const ParquetChunkMeta &chunk = meta_.chunks_.at(group.first_chunk_ + col.source_idx_);
    if (OB_FAIL(readers_[k].init(file_, desc_->path_, col, chunk))) {
    }
  }
  if (OB_SUCC(ret)) {
    rows_left_ = group.num_rows_;
  }
  return ret;
}

int ParquetRowSource::next_row(ObIAllocator &alloc, FileValue *values)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && rows_left_ <= 0) {
    if (group_idx_ + 1 >= meta_.row_groups_.count()) {
      if (OB_SUCC(file_.check_unchanged())) {
        ret = OB_ITER_END;
      }
    } else if (OB_FAIL(open_row_group(++group_idx_))) {
    }
  }
  for (int64_t k = 0; OB_SUCC(ret) && k < reader_count_; ++k) {
    FileValue &value = values[k];
    if (OB_FAIL(readers_[k].next(value))) {
    } else if (!value.is_null_ && !value.str_.empty()) {
      ObString copied;
      if (OB_FAIL(ob_write_string(alloc, value.str_, copied))) {
      } else {
        value.str_ = copied;
      }
    }
  }
  if (OB_SUCC(ret)) {
    --rows_left_;
  }
  return ret;
}

int ParquetRowSource::rewind()
{
  group_idx_ = -1;
  rows_left_ = 0;
  return OB_SUCCESS;
}

void ParquetRowSource::close()
{
  for (int64_t k = 0; NULL != readers_ && k < reader_count_; ++k) {
    readers_[k].~ParquetColumnReader();
  }
  readers_ = NULL;
  reader_count_ = 0;
  meta_.reset();
  meta_alloc_.reset();
  file_.close();
  group_idx_ = -1;
  rows_left_ = 0;
}

} // namespace sql
} // namespace oceanbase
