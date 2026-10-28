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

// File SQL: read_csv / read_json / read_parquet / glob table functions and COPY TO.
// This header holds the format independent pieces: table descriptor, file identity,
// path security checks, value classification and conversion, the streaming CSV and
// newline-delimited JSON readers, schema inference, glob and the CSV exporter.

#pragma once

#include "lib/allocator/page_arena.h"
#include "lib/hash/ob_hashmap.h"
#include "lib/container/ob_fixed_array.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_sql_string.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/ob_unify_serialize.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;

enum class FileFormat : int8_t
{
  INVALID = 0,
  CSV = 1,
  JSON = 2,
  PARQUET = 3,
  GLOB = 4,
};

// Inferred column types. The order matters for widening: see widen_file_column_type().
enum class FileColumnType : int8_t
{
  NULL_TYPE = 0,
  BOOLEAN = 1,
  BIGINT = 2,
  DOUBLE = 3,
  DATE = 4,
  DATETIME = 5,
  VARCHAR = 6,
};

const char *file_format_name(const FileFormat format);
const char *file_column_type_name(const FileColumnType type);
// Deterministic widening lattice:
//   NULL + X = X, X + X = X, BIGINT + DOUBLE = DOUBLE, DATE + DATETIME = DATETIME,
//   anything else = VARCHAR.
FileColumnType widen_file_column_type(const FileColumnType a, const FileColumnType b);

struct FileSqlLimits
{
  static const int64_t MAX_COLUMN_COUNT = 4096;
  static const int64_t DEFAULT_MAX_LINE_SIZE = 2L * 1024 * 1024;
  static const int64_t MAX_LINE_SIZE_LIMIT = 64L * 1024 * 1024;
  static const int64_t DEFAULT_MAX_OBJECT_SIZE = 16L * 1024 * 1024;
  // VARCHAR values longer than this many characters are rejected.
  static const int64_t MAX_VARCHAR_CHARS = 262144;
  static const int64_t MAX_COLUMN_NAME_CHARS = 64;
  static const int64_t MAX_GLOB_FILES = 100000;
  static const int64_t MAX_GLOB_DEPTH = 64;
  static const int64_t READ_BLOCK_SIZE = 256L * 1024;
  static const int64_t CHECK_STATUS_INTERVAL = 4096;
};

// Identity of an input file. A scan fails with OB_TABLE_DEFINITION_CHANGED when the file
// it opens is not the one whose schema was inferred.
struct FileIdentity
{
  OB_UNIS_VERSION(1);
public:
  FileIdentity() : dev_(0), ino_(0), size_(-1), mtime_ns_(0) {}
  bool is_valid() const { return size_ >= 0; }
  bool equals(const FileIdentity &other) const
  {
    return dev_ == other.dev_ && ino_ == other.ino_ && size_ == other.size_
        && mtime_ns_ == other.mtime_ns_;
  }
  TO_STRING_KV(K_(dev), K_(ino), K_(size), K_(mtime_ns));
  int64_t dev_;
  int64_t ino_;
  int64_t size_;
  int64_t mtime_ns_;
};

struct FileColumnDesc
{
  OB_UNIS_VERSION(1);
public:
  FileColumnDesc()
    : name_(), source_name_(), type_(static_cast<int8_t>(FileColumnType::NULL_TYPE)),
      nullable_(true), max_length_(1), scale_(0), source_idx_(0), pq_physical_type_(-1),
      pq_value_kind_(0), pq_scale_(0), pq_type_length_(0), pq_max_def_level_(0)
  {}
  FileColumnType get_type() const { return static_cast<FileColumnType>(type_); }
  void set_type(const FileColumnType type) { type_ = static_cast<int8_t>(type); }
  TO_STRING_KV(K_(name), K_(source_name), K_(type), K_(nullable), K_(max_length), K_(scale),
               K_(source_idx), K_(pq_physical_type), K_(pq_value_kind), K_(pq_scale));
  common::ObString name_;         // SQL column name, unique case-insensitively
  common::ObString source_name_;  // header text / JSON key / parquet field name
  int8_t type_;                   // FileColumnType
  bool nullable_;
  int64_t max_length_;            // VARCHAR length in characters
  int16_t scale_;                 // DATETIME fractional second digits
  int32_t source_idx_;            // CSV field index / parquet leaf column index
  // parquet decoding information, see ob_file_parquet_reader.h
  int32_t pq_physical_type_;
  int32_t pq_value_kind_;
  int32_t pq_scale_;
  int32_t pq_type_length_;
  int32_t pq_max_def_level_;
};

typedef common::ObFixedArray<FileColumnDesc, common::ObIAllocator> FileColumnArray;

// Everything the scan needs to know about one file table. It is built and inferred by the
// resolver, serialized into the operator spec and deserialized when the scan opens.
struct FileTableDesc
{
  OB_UNIS_VERSION(1);
public:
  explicit FileTableDesc(common::ObIAllocator &alloc)
    : format_(static_cast<int8_t>(FileFormat::INVALID)), func_name_(), path_(), real_path_(),
      identity_(), delimiter_(','), quote_('"'), escape_('"'), header_option_(-1),
      has_header_(false), null_str_(), skip_rows_(0), all_varchar_(false),
      ignore_errors_(false), max_line_size_(FileSqlLimits::DEFAULT_MAX_LINE_SIZE),
      max_object_size_(FileSqlLimits::DEFAULT_MAX_OBJECT_SIZE), row_count_(0),
      rejected_rows_(0), columns_(alloc)
  {}
  FileFormat get_format() const { return static_cast<FileFormat>(format_); }
  void set_format(const FileFormat format) { format_ = static_cast<int8_t>(format); }
  int assign(const FileTableDesc &other, common::ObIAllocator &alloc);
  int64_t get_column_count() const { return columns_.count(); }
  TO_STRING_KV(K_(format), K_(func_name), K_(path), K_(real_path), K_(identity), K_(delimiter),
               K_(quote), K_(escape), K_(has_header), K_(null_str), K_(skip_rows),
               K_(all_varchar), K_(ignore_errors), K_(row_count), K_(columns));
  int8_t format_;
  common::ObString func_name_;
  common::ObString path_;        // as written by the user
  common::ObString real_path_;   // canonical path, or canonical pattern for glob
  FileIdentity identity_;
  // CSV options
  int8_t delimiter_;             // 0: detected by the inference
  int8_t quote_;                 // 0 means no quoting
  int8_t escape_;                // 0 means no escaping
  int8_t header_option_;         // -1 auto detect, 0 no header, 1 header
  bool has_header_;              // resolved header flag
  common::ObString null_str_;    // unquoted value treated as NULL ('' by default)
  int64_t skip_rows_;
  bool all_varchar_;
  bool ignore_errors_;
  int64_t max_line_size_;
  // JSON options
  int64_t max_object_size_;
  // inference results
  int64_t row_count_;
  int64_t rejected_rows_;
  FileColumnArray columns_;
};

// One value produced by a file scan, already converted to the column type.
struct FileValue
{
  FileValue() : is_null_(true), int_(0), double_(0.0), str_() {}
  void reset() { is_null_ = true; int_ = 0; double_ = 0.0; str_.reset(); }
  bool is_null_;
  int64_t int_;          // BIGINT, BOOLEAN, DATE (days), DATETIME (usec)
  double double_;
  common::ObString str_; // VARCHAR, owned by the scan's row allocator
};

class FileSqlUtil
{
public:
  // ---- paths and security ----
  // Resolve a user path to a canonical absolute path. The parent directory must exist.
  // When must_exist is false, a missing final component is allowed (used by COPY TO).
  static int canonicalize_path(const common::ObString &path, const bool must_exist,
                               common::ObIAllocator &alloc, common::ObString &real_path,
                               bool &exists);
  // Strict secure_file_priv check: empty / NULL denies everything, otherwise the canonical
  // path must equal the canonical directory or be inside it.
  static int check_secure_path(const common::ObString &secure_file_priv,
                               const common::ObString &real_path);
  static bool is_path_allowed(const common::ObString &secure_file_priv,
                              const common::ObString &real_path);
  // Canonicalize a file to read and check secure_file_priv before existence, so a path
  // outside secure_file_priv is always reported as a privilege error.
  static int resolve_input_path(const common::ObString &path,
                                const common::ObString &secure_file_priv,
                                common::ObIAllocator &alloc, common::ObString &real_path);
  // Same for a COPY target, which must not exist yet.
  static int resolve_output_path(const common::ObString &path,
                                 const common::ObString &secure_file_priv,
                                 common::ObIAllocator &alloc, common::ObString &real_path);
  // Open a regular file read-only, without following a symlink in the last component.
  static int open_read_only(const common::ObString &real_path, int &fd, FileIdentity &identity);
  static int get_fd_identity(const int fd, FileIdentity &identity);
  // Format from extension: .csv/.tsv/.tab/.txt? -> CSV only through read_csv, etc.
  static int check_extension(const FileFormat format, const common::ObString &path);

  // ---- text helpers ----
  static bool is_valid_utf8(const char *ptr, const int64_t len);
  static int64_t utf8_char_count(const char *ptr, const int64_t len);
  // Classify a non-null text value for inference. scale is set for DATETIME.
  static FileColumnType classify_text(const char *ptr, const int64_t len, int16_t &scale);
  // Classify a JSON string value (only DATE / DATETIME / VARCHAR).
  static FileColumnType classify_json_string(const char *ptr, const int64_t len, int16_t &scale);
  static bool parse_bool(const char *ptr, const int64_t len, bool &value);
  static bool parse_bigint(const char *ptr, const int64_t len, int64_t &value);
  static bool parse_double(const char *ptr, const int64_t len, double &value);
  // DATE value in days since 1970-01-01, DATETIME in microseconds since 1970-01-01 00:00:00.
  static bool parse_date(const char *ptr, const int64_t len, int64_t &days);
  static bool parse_datetime(const char *ptr, const int64_t len, int64_t &usec, int16_t &scale);
  static int64_t days_from_civil(int64_t y, const int64_t m, const int64_t d);
  static bool is_valid_date(const int64_t y, const int64_t m, const int64_t d);
  // Convert a text value to a column type; returns false when the text does not fit.
  static bool convert_text(const FileColumnType type, const char *ptr, const int64_t len,
                           FileValue &value);
  // Build unique column names: empty -> column<N>, duplicates -> name__<K>.
  static int make_unique_names(common::ObIAllocator &alloc, FileColumnArray &columns);
  static int set_user_error(const int err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
  // ---- runtime settings ----
  // enable_file_sql must be ON in the session.
  static int check_enabled(const ObSQLSessionInfo &session);
  // Latest global secure_file_priv.
  static int get_secure_file_priv(common::ObIAllocator &alloc, common::ObString &secure_file_priv);
};

// Read-only local file with sequential and positional reads. The visible size is fixed at
// open: data appended later is never read and the identity is checked again at the end.
class LocalFileReader
{
public:
  LocalFileReader() : fd_(-1), identity_(), offset_(0), path_() {}
  ~LocalFileReader() { close(); }
  int open(const common::ObString &real_path, const FileIdentity *expected);
  int read(char *buf, const int64_t len, int64_t &read_len);
  int pread(char *buf, const int64_t len, const int64_t offset, int64_t &read_len);
  int rewind() { offset_ = 0; return common::OB_SUCCESS; }
  int check_unchanged() const;
  void close();
  bool is_opened() const { return fd_ >= 0; }
  int64_t get_size() const { return identity_.size_; }
  const FileIdentity &get_identity() const { return identity_; }
  const common::ObString &get_path() const { return path_; }
private:
  int fd_;
  FileIdentity identity_;
  int64_t offset_;
  common::ObString path_;
  DISALLOW_COPY_AND_ASSIGN(LocalFileReader);
};

struct CsvField
{
  CsvField() : ptr_(NULL), len_(0), quoted_(false) {}
  CsvField(const char *ptr, const int64_t len, const bool quoted)
    : ptr_(ptr), len_(len), quoted_(quoted) {}
  TO_STRING_KV(K_(len), K_(quoted));
  const char *ptr_;
  int64_t len_;
  bool quoted_;
};

// Streaming CSV record reader. It keeps one block plus the current record in memory.
// Records may contain quoted delimiters and newlines; LF, CRLF and CR end a record.
class CsvRecordReader
{
public:
  CsvRecordReader();
  ~CsvRecordReader() { destroy(); }
  int init(LocalFileReader &file, const FileTableDesc &desc);
  // Returns OB_ITER_END at end of file. Fields stay valid until the next call.
  // line_no is the 1-based physical line where the record starts.
  int next_record(common::ObIArray<CsvField> &fields, int64_t &line_no);
  int rewind();
  void destroy();
private:
  int ensure_data(const int64_t need_pos, bool &has_more);
  int find_record_end(int64_t &rec_end, int64_t &next_pos, int64_t &newlines);
  int split_fields(const int64_t rec_end, common::ObIArray<CsvField> &fields);
private:
  LocalFileReader *file_;
  char delimiter_;
  char quote_;
  char escape_;
  int64_t max_line_size_;
  char *buf_;
  int64_t buf_cap_;
  int64_t pos_;
  int64_t end_;
  bool eof_;
  bool bom_checked_;
  int64_t line_no_;
  DISALLOW_COPY_AND_ASSIGN(CsvRecordReader);
};

// Newline-delimited reader used by the JSON scan. CR before LF is dropped.
class LineReader
{
public:
  LineReader();
  ~LineReader() { destroy(); }
  int init(LocalFileReader &file, const int64_t max_line_size);
  int next_line(const char *&ptr, int64_t &len, int64_t &line_no);
  int rewind();
  void destroy();
private:
  LocalFileReader *file_;
  int64_t max_line_size_;
  char *buf_;
  int64_t buf_cap_;
  int64_t pos_;
  int64_t end_;
  bool eof_;
  bool bom_checked_;
  int64_t line_no_;
  DISALLOW_COPY_AND_ASSIGN(LineReader);
};

enum class JsonValueKind : int8_t
{
  NONE = 0,
  NULL_VALUE = 1,
  BOOL_TRUE = 2,
  BOOL_FALSE = 3,
  INTEGER = 4,   // integer literal that fits BIGINT
  NUMBER = 5,    // any other number
  STRING = 6,
};

struct JsonField
{
  JsonField() : key_(), kind_(JsonValueKind::NONE), text_() {}
  TO_STRING_KV(K_(key), K_(kind), K_(text));
  common::ObString key_;
  JsonValueKind kind_;
  common::ObString text_;  // unescaped string or number literal
};

// Parser for one flat JSON object per line. Nested objects and arrays are rejected.
class JsonLineParser
{
public:
  JsonLineParser() : scratch_("FileSqlJson") {}
  // Parses one line. Strings with escapes are unescaped into an internal buffer that is
  // reused by the next call.
  int parse(const char *ptr, const int64_t len, const int64_t line_no,
            common::ObIArray<JsonField> &fields);
  static int json_value_type(const JsonField &field, FileColumnType &type, int16_t &scale);
  static bool convert_json(const FileColumnType type, const JsonField &field, FileValue &value);
private:
  int parse_string(const char *ptr, const int64_t len, int64_t &pos, const int64_t line_no,
                   common::ObString &str);
  int parse_number(const char *ptr, const int64_t len, int64_t &pos, const int64_t line_no,
                   JsonField &field);
  int parse_value(const char *ptr, const int64_t len, int64_t &pos, const int64_t line_no,
                  JsonField &field);
private:
  common::ObArenaAllocator scratch_;
};

// Per-column inference state shared by CSV and JSON.
struct FileColumnInferState
{
  FileColumnInferState()
    : type_(FileColumnType::NULL_TYPE), has_null_(false), has_value_(false), max_chars_(0),
      max_scale_(0) {}
  void add_null() { has_null_ = true; }
  void add_value(const FileColumnType type, const int64_t chars, const int16_t scale)
  {
    type_ = widen_file_column_type(type_, type);
    has_value_ = true;
    max_chars_ = max_chars_ > chars ? max_chars_ : chars;
    if (FileColumnType::DATETIME == type && scale > max_scale_) {
      max_scale_ = scale;
    }
  }
  void merge(const FileColumnInferState &other)
  {
    if (other.has_value_) {
      type_ = widen_file_column_type(type_, other.type_);
      has_value_ = true;
      max_chars_ = max_chars_ > other.max_chars_ ? max_chars_ : other.max_chars_;
      max_scale_ = max_scale_ > other.max_scale_ ? max_scale_ : other.max_scale_;
    }
    has_null_ = has_null_ || other.has_null_;
  }
  TO_STRING_KV(K_(type), K_(has_null), K_(has_value), K_(max_chars), K_(max_scale));
  FileColumnType type_;
  bool has_null_;
  bool has_value_;
  int64_t max_chars_;
  int16_t max_scale_;
};

class FileSchemaInferrer
{
public:
  // Full scan inference (sample_size = -1). desc must have path, identity and options set.
  static int infer_csv(common::ObIAllocator &alloc, FileTableDesc &desc);
  static int infer_json(common::ObIAllocator &alloc, FileTableDesc &desc);
  static int infer_glob(common::ObIAllocator &alloc, const common::ObString &secure_file_priv,
                        FileTableDesc &desc);
  static void fill_column(const FileColumnInferState &state, const bool all_varchar,
                          FileColumnDesc &column);
};

// Recursive file discovery for glob(): '*', '?', '[...]' within one path component and
// '**' for any number of directories. Only regular files inside secure_file_priv are listed.
class FileGlob
{
public:
  static int glob(const common::ObString &pattern, const common::ObString &secure_file_priv,
                  common::ObIAllocator &alloc, common::ObIArray<common::ObString> &files);
  static bool has_wildcard(const common::ObString &str);
};

// COPY ... TO csv options. They are carried to the executor inside
// ObSelectIntoItem::external_properties_ with FILE_SQL_COPY_PREFIX.
struct CsvExportOptions
{
  CsvExportOptions()
    : delimiter_(','), quote_('"'), escape_('"'), header_(false), null_str_(), column_names_()
  {}
  int encode(common::ObIAllocator &alloc, common::ObString &out) const;
  int decode(const common::ObString &in);
  static bool is_copy_properties(const common::ObString &properties);
  TO_STRING_KV(K_(delimiter), K_(quote), K_(escape), K_(header), K_(null_str));
  char delimiter_;
  char quote_;
  char escape_;
  bool header_;
  common::ObString null_str_;
  common::ObSEArray<common::ObString, 16> column_names_;
};

// Writes CSV into a temporary file next to the target and publishes it with link(), so the
// target is never overwritten and a failed query leaves no file behind.
class CsvFileExporter
{
public:
  CsvFileExporter();
  ~CsvFileExporter() { abort(); }
  int open(const common::ObString &target_real_path, const CsvExportOptions &options);
  int write_header();     // no-op unless the header option is set
  int write_field(const char *ptr, const int64_t len, const bool is_null, const bool is_first);
  int end_row();
  int finish();
  void abort();
  bool is_opened() const { return fd_ >= 0; }
  bool is_finished() const { return finished_; }
  int64_t get_row_count() const { return row_count_; }
private:
  int append(const char *ptr, const int64_t len);
  int flush();
  bool need_quote(const char *ptr, const int64_t len) const;
private:
  int fd_;
  CsvExportOptions options_;
  char target_path_[4096 + 1];
  char tmp_path_[4096 + 1];
  char *buf_;
  int64_t buf_len_;
  int64_t buf_pos_;
  int64_t row_count_;
  bool finished_;
  DISALLOW_COPY_AND_ASSIGN(CsvFileExporter);
};

// Produces the rows of one file table, converted to the inferred column types.
class IFileRowSource
{
public:
  virtual ~IFileRowSource() {}
  // column_idxs are indexes into desc.columns_ in output order; desc must outlive the source.
  virtual int open(const FileTableDesc &desc, const common::ObIArray<int64_t> &column_idxs,
                   const common::ObString &secure_file_priv) = 0;
  // Fills one value per projected column, VARCHAR values are copied into alloc. Returns
  // OB_ITER_END after the last row, once the file is known to be unchanged.
  virtual int next_row(common::ObIAllocator &alloc, FileValue *values) = 0;
  virtual int rewind() = 0;
  virtual void close() = 0;
};

class CsvRowSource : public IFileRowSource
{
public:
  CsvRowSource() : desc_(NULL), file_(), reader_(), fields_(), column_idxs_() {}
  virtual ~CsvRowSource() { close(); }
  virtual int open(const FileTableDesc &desc, const common::ObIArray<int64_t> &column_idxs,
                   const common::ObString &secure_file_priv) override;
  virtual int next_row(common::ObIAllocator &alloc, FileValue *values) override;
  virtual int rewind() override;
  virtual void close() override;
private:
  int skip_leading_records();
private:
  const FileTableDesc *desc_;
  LocalFileReader file_;
  CsvRecordReader reader_;
  common::ObSEArray<CsvField, 64> fields_;
  common::ObSEArray<int64_t, 16> column_idxs_;
  DISALLOW_COPY_AND_ASSIGN(CsvRowSource);
};

class JsonRowSource : public IFileRowSource
{
public:
  JsonRowSource()
    : desc_(NULL), file_(), reader_(), parser_(), fields_(), column_idxs_(), key_map_() {}
  virtual ~JsonRowSource() { close(); }
  virtual int open(const FileTableDesc &desc, const common::ObIArray<int64_t> &column_idxs,
                   const common::ObString &secure_file_priv) override;
  virtual int next_row(common::ObIAllocator &alloc, FileValue *values) override;
  virtual int rewind() override;
  virtual void close() override;
private:
  const FileTableDesc *desc_;
  LocalFileReader file_;
  LineReader reader_;
  JsonLineParser parser_;
  common::ObSEArray<JsonField, 32> fields_;
  common::ObSEArray<int64_t, 16> column_idxs_;
  // JSON key -> position in column_idxs_
  common::hash::ObHashMap<common::ObString, int64_t, common::hash::NoPthreadDefendMode> key_map_;
  DISALLOW_COPY_AND_ASSIGN(JsonRowSource);
};

// glob() lists the files when the scan opens, so it always reflects the directory content.
class GlobRowSource : public IFileRowSource
{
public:
  GlobRowSource() : alloc_("FileSqlGlob"), files_(), column_count_(0), idx_(0) {}
  virtual ~GlobRowSource() { close(); }
  virtual int open(const FileTableDesc &desc, const common::ObIArray<int64_t> &column_idxs,
                   const common::ObString &secure_file_priv) override;
  virtual int next_row(common::ObIAllocator &alloc, FileValue *values) override;
  virtual int rewind() override { idx_ = 0; return common::OB_SUCCESS; }
  virtual void close() override;
private:
  common::ObArenaAllocator alloc_;
  common::ObSEArray<common::ObString, 64> files_;
  int64_t column_count_;
  int64_t idx_;
  DISALLOW_COPY_AND_ASSIGN(GlobRowSource);
};

extern const char *const FILE_SQL_COPY_PREFIX;

} // namespace sql
} // namespace oceanbase
