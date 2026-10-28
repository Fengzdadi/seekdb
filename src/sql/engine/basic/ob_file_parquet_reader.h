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

// read_parquet: footer schema and a streaming reader of the projected column chunks.
//
// Supported: flat schemas of primitive columns (REQUIRED / OPTIONAL), any number of row
// groups, data pages v1 and v2, PLAIN, dictionary (PLAIN_DICTIONARY / RLE_DICTIONARY) and
// RLE boolean encodings, RLE definition levels and UNCOMPRESSED / SNAPPY / GZIP / ZSTD
// column chunks.
// Column types map to the file sql types: BOOLEAN; INT32 / INT64 (and INT annotations)
// -> BIGINT; FLOAT / DOUBLE / DECIMAL -> DOUBLE; DATE; TIMESTAMP and INT96 -> DATETIME;
// BYTE_ARRAY -> VARCHAR (values must be utf8mb4).

#pragma once

#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

// How a parquet value is converted, stored in FileColumnDesc::pq_value_kind_.
enum class ParquetValueKind : int32_t
{
  INVALID = 0,
  BOOL = 1,
  INT = 2,          // signed INT32 / INT64
  UINT32 = 3,
  UINT64 = 4,
  DATE = 5,         // INT32 days
  TS_MILLIS = 6,
  TS_MICROS = 7,
  TS_NANOS = 8,
  INT96 = 9,        // legacy Impala timestamp
  FLOAT = 10,
  DOUBLE = 11,
  STRING = 12,
  DECIMAL = 13,     // unscaled INT32 / INT64 / BYTE_ARRAY / FIXED_LEN_BYTE_ARRAY
};

// Parquet physical types.
enum ParquetPhysicalType
{
  PQ_BOOLEAN = 0,
  PQ_INT32 = 1,
  PQ_INT64 = 2,
  PQ_INT96 = 3,
  PQ_FLOAT = 4,
  PQ_DOUBLE = 5,
  PQ_BYTE_ARRAY = 6,
  PQ_FIXED_LEN_BYTE_ARRAY = 7,
};

struct ParquetChunkMeta
{
  ParquetChunkMeta()
    : physical_type_(-1), codec_(0), num_values_(0), data_page_offset_(-1),
      dictionary_page_offset_(-1), total_compressed_size_(0) {}
  TO_STRING_KV(K_(physical_type), K_(codec), K_(num_values), K_(data_page_offset),
               K_(dictionary_page_offset), K_(total_compressed_size));
  int32_t physical_type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  int64_t total_compressed_size_;
};

struct ParquetRowGroupMeta
{
  ParquetRowGroupMeta() : num_rows_(0), first_chunk_(0), chunk_count_(0) {}
  TO_STRING_KV(K_(num_rows), K_(first_chunk), K_(chunk_count));
  int64_t num_rows_;
  int64_t first_chunk_;   // index into ParquetFileMeta::chunks_
  int64_t chunk_count_;
};

struct ParquetLeafMeta
{
  ParquetLeafMeta()
    : name_(), physical_type_(-1), type_length_(0), repetition_(-1), converted_type_(-1),
      scale_(0), precision_(0), logical_kind_(0), time_unit_(0), int_bit_width_(0),
      int_signed_(true) {}
  TO_STRING_KV(K_(name), K_(physical_type), K_(type_length), K_(repetition),
               K_(converted_type), K_(scale), K_(precision), K_(logical_kind), K_(time_unit));
  common::ObString name_;
  int32_t physical_type_;
  int32_t type_length_;
  int32_t repetition_;
  int32_t converted_type_;
  int32_t scale_;
  int32_t precision_;
  int32_t logical_kind_;   // LogicalType union field id, 0 when absent
  int32_t time_unit_;      // 1 millis, 2 micros, 3 nanos
  int32_t int_bit_width_;
  bool int_signed_;
};

struct ParquetFileMeta
{
  ParquetFileMeta() : num_rows_(0), leaves_(), row_groups_(), chunks_() {}
  void reset() { num_rows_ = 0; leaves_.reset(); row_groups_.reset(); chunks_.reset(); }
  int64_t num_rows_;
  common::ObSEArray<ParquetLeafMeta, 16> leaves_;
  common::ObSEArray<ParquetRowGroupMeta, 4> row_groups_;
  common::ObSEArray<ParquetChunkMeta, 16> chunks_;
};

class ParquetUtil
{
public:
  // Read and parse the footer. Strings in meta point into alloc.
  static int read_file_meta(LocalFileReader &file, const common::ObString &path,
                            common::ObIAllocator &alloc, ParquetFileMeta &meta);
  // Raw snappy block format, as used by parquet.
  static int snappy_uncompress(const char *src, const int64_t src_len, char *dst,
                               const int64_t dst_cap, int64_t &dst_len);
  static int gzip_uncompress(const char *src, const int64_t src_len, char *dst,
                             const int64_t dst_cap, int64_t &dst_len);
  static int zstd_uncompress(const char *src, const int64_t src_len, char *dst,
                             const int64_t dst_cap, int64_t &dst_len);
};

// Infer the columns of a parquet file from its footer. desc must have path and identity.
int infer_parquet_schema(common::ObIAllocator &alloc, FileTableDesc &desc);

// Run length / bit packed hybrid decoder used for levels and dictionary indexes.
class ParquetRleDecoder
{
public:
  ParquetRleDecoder() { reset(); }
  void reset()
  {
    data_ = NULL; len_ = 0; pos_ = 0; bit_width_ = 0; rle_left_ = 0; rle_value_ = 0;
    bp_left_ = 0; bp_bit_pos_ = 0;
  }
  void init(const uint8_t *data, const int64_t len, const int32_t bit_width)
  {
    reset();
    data_ = data; len_ = len; bit_width_ = bit_width;
  }
  int next(uint32_t &value);
private:
  int next_run();
private:
  const uint8_t *data_;
  int64_t len_;
  int64_t pos_;
  int32_t bit_width_;
  int64_t rle_left_;
  uint32_t rle_value_;
  int64_t bp_left_;
  int64_t bp_bit_pos_;    // absolute bit position in data_
};

// Values of one projected column chunk, page by page.
class ParquetColumnReader
{
public:
  ParquetColumnReader();
  ~ParquetColumnReader() { reset(); }
  int init(LocalFileReader &file, const common::ObString &path, const FileColumnDesc &column,
           const ParquetChunkMeta &chunk);
  int next(FileValue &value);
  void reset();
private:
  int read_page_header(const int64_t offset, int32_t &page_type, int32_t &uncompressed_size,
                       int32_t &compressed_size, int32_t &num_values, int32_t &encoding,
                       int32_t &def_encoding, int32_t &def_len, int32_t &rep_len,
                       bool &is_compressed, int64_t &header_len);
  int load_page();
  int read_bytes(const int64_t offset, const int64_t len, char *&buf);
  int uncompress(const char *src, const int64_t src_len, const int64_t dst_len, char *&dst);
  int load_dictionary(const char *data, const int64_t len, const int32_t num_values);
  int decode_plain(const char *&ptr, const char *end, FileValue &value);
  int convert_int(const int64_t v, FileValue &value);
  int convert_bytes(const char *ptr, const int64_t len, FileValue &value);
  int corrupt(const char *what) const;
  int ensure_buf(char *&buf, int64_t &cap, const int64_t need);
private:
  common::ObArenaAllocator alloc_;
  LocalFileReader *file_;
  common::ObString path_;
  const FileColumnDesc *column_;
  ParquetChunkMeta chunk_;
  int64_t next_page_offset_;
  int64_t chunk_end_;
  int64_t values_read_;           // values of the chunk handed out so far
  // raw page bytes and uncompressed page
  char *read_buf_;
  int64_t read_buf_cap_;
  char *page_buf_;
  int64_t page_buf_cap_;
  // current data page
  int64_t page_values_;
  int64_t page_value_idx_;
  bool has_def_levels_;
  ParquetRleDecoder def_decoder_;
  bool dict_encoded_;
  bool rle_bool_;                 // booleans of the page are run length encoded
  ParquetRleDecoder index_decoder_;   // dictionary indexes or run length encoded booleans
  const char *value_ptr_;
  const char *value_end_;
  int64_t bool_bit_idx_;
  // dictionary
  FileValue *dict_;
  int64_t dict_count_;
  DISALLOW_COPY_AND_ASSIGN(ParquetColumnReader);
};

class ParquetRowSource : public IFileRowSource
{
public:
  ParquetRowSource();
  virtual ~ParquetRowSource() { close(); }
  virtual int open(const FileTableDesc &desc, const common::ObIArray<int64_t> &column_idxs,
                   const common::ObString &secure_file_priv) override;
  virtual int next_row(common::ObIAllocator &alloc, FileValue *values) override;
  virtual int rewind() override;
  virtual void close() override;
private:
  int open_row_group(const int64_t group_idx);
private:
  common::ObArenaAllocator meta_alloc_;
  const FileTableDesc *desc_;
  LocalFileReader file_;
  ParquetFileMeta meta_;
  common::ObSEArray<int64_t, 16> column_idxs_;
  ParquetColumnReader *readers_;
  int64_t reader_count_;
  int64_t group_idx_;
  int64_t rows_left_;
  DISALLOW_COPY_AND_ASSIGN(ParquetRowSource);
};

} // namespace sql
} // namespace oceanbase
