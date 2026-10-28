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

// Readers behind read_csv / read_json / read_parquet: CSV inference and options, JSONL field
// union, file change detection, path checks, COPY export and parquet decoding.

#define USING_LOG_PREFIX SQL_ENG

#include <gtest/gtest.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "lib/allocator/page_arena.h"
#include "share/ob_errno.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/ob_file_parquet_reader.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace file_scan_utils_test
{

// Parquet files written by tools/deploy/mysql_test/test_suite/file_sql/gen_parquet_fixtures.py:
//   SNAPPY_V1_PARQUET  id INT64 REQUIRED [1..5], name STRING dictionary [a, NULL, bb, a, ccc],
//                      ok BOOLEAN [true, false, NULL, true, false]; row groups of 3 and 2 rows
//   GZIP_V2_PARQUET    v DOUBLE [0.5, NULL, -2, 4.25], day DATE [2024-01-01, 1970-01-01, NULL,
//                      1969-12-31]; one row group of data page v2
const unsigned char SNAPPY_V1_PARQUET[] = {
  0x50, 0x41, 0x52, 0x31, 0x15, 0x00, 0x15, 0x30, 0x15, 0x1e, 0x2c, 0x15, 0x06, 0x15, 0x00, 0x15,
  0x06, 0x15, 0x06, 0x00, 0x00, 0x18, 0x04, 0x01, 0x00, 0x09, 0x01, 0x00, 0x02, 0x09, 0x07, 0x04,
  0x00, 0x03, 0x0d, 0x08, 0x15, 0x04, 0x15, 0x16, 0x15, 0x1a, 0x4c, 0x15, 0x04, 0x15, 0x00, 0x00,
  0x00, 0x0b, 0x28, 0x01, 0x00, 0x00, 0x00, 0x61, 0x02, 0x00, 0x00, 0x00, 0x62, 0x62, 0x15, 0x00,
  0x15, 0x12, 0x15, 0x16, 0x2c, 0x15, 0x06, 0x15, 0x10, 0x15, 0x06, 0x15, 0x06, 0x00, 0x00, 0x09,
  0x20, 0x02, 0x00, 0x00, 0x00, 0x03, 0x05, 0x01, 0x03, 0x02, 0x15, 0x00, 0x15, 0x0e, 0x15, 0x12,
  0x2c, 0x15, 0x06, 0x15, 0x00, 0x15, 0x06, 0x15, 0x06, 0x00, 0x00, 0x07, 0x18, 0x02, 0x00, 0x00,
  0x00, 0x03, 0x03, 0x01, 0x15, 0x00, 0x15, 0x20, 0x15, 0x18, 0x2c, 0x15, 0x04, 0x15, 0x00, 0x15,
  0x06, 0x15, 0x06, 0x00, 0x00, 0x10, 0x04, 0x04, 0x00, 0x09, 0x01, 0x00, 0x05, 0x09, 0x07, 0x00,
  0x00, 0x15, 0x04, 0x15, 0x18, 0x15, 0x1c, 0x4c, 0x15, 0x04, 0x15, 0x00, 0x00, 0x00, 0x0c, 0x2c,
  0x01, 0x00, 0x00, 0x00, 0x61, 0x03, 0x00, 0x00, 0x00, 0x63, 0x63, 0x63, 0x15, 0x00, 0x15, 0x12,
  0x15, 0x16, 0x2c, 0x15, 0x04, 0x15, 0x10, 0x15, 0x06, 0x15, 0x06, 0x00, 0x00, 0x09, 0x20, 0x02,
  0x00, 0x00, 0x00, 0x03, 0x03, 0x01, 0x03, 0x02, 0x15, 0x00, 0x15, 0x0e, 0x15, 0x12, 0x2c, 0x15,
  0x04, 0x15, 0x00, 0x15, 0x06, 0x15, 0x06, 0x00, 0x00, 0x07, 0x18, 0x02, 0x00, 0x00, 0x00, 0x03,
  0x03, 0x01, 0x15, 0x02, 0x19, 0x4c, 0x48, 0x06, 0x73, 0x63, 0x68, 0x65, 0x6d, 0x61, 0x15, 0x06,
  0x00, 0x15, 0x04, 0x25, 0x00, 0x18, 0x02, 0x69, 0x64, 0x00, 0x15, 0x0c, 0x25, 0x02, 0x18, 0x04,
  0x6e, 0x61, 0x6d, 0x65, 0x25, 0x00, 0x4c, 0x1c, 0x00, 0x00, 0x00, 0x15, 0x00, 0x25, 0x02, 0x18,
  0x02, 0x6f, 0x6b, 0x00, 0x16, 0x0a, 0x19, 0x2c, 0x19, 0x3c, 0x26, 0x08, 0x1c, 0x15, 0x04, 0x19,
  0x25, 0x00, 0x06, 0x19, 0x18, 0x02, 0x69, 0x64, 0x15, 0x02, 0x16, 0x06, 0x16, 0x52, 0x16, 0x40,
  0x26, 0x08, 0x00, 0x00, 0x26, 0x48, 0x1c, 0x15, 0x0c, 0x19, 0x35, 0x00, 0x06, 0x10, 0x19, 0x18,
  0x04, 0x6e, 0x61, 0x6d, 0x65, 0x15, 0x02, 0x16, 0x06, 0x16, 0x64, 0x16, 0x6c, 0x26, 0x7c, 0x26,
  0x48, 0x00, 0x00, 0x26, 0xb4, 0x01, 0x1c, 0x15, 0x00, 0x19, 0x25, 0x00, 0x06, 0x19, 0x18, 0x02,
  0x6f, 0x6b, 0x15, 0x02, 0x16, 0x06, 0x16, 0x30, 0x16, 0x34, 0x26, 0xb4, 0x01, 0x00, 0x00, 0x16,
  0xe0, 0x01, 0x16, 0x06, 0x00, 0x19, 0x3c, 0x26, 0xe8, 0x01, 0x1c, 0x15, 0x04, 0x19, 0x25, 0x00,
  0x06, 0x19, 0x18, 0x02, 0x69, 0x64, 0x15, 0x02, 0x16, 0x04, 0x16, 0x42, 0x16, 0x3a, 0x26, 0xe8,
  0x01, 0x00, 0x00, 0x26, 0xa2, 0x02, 0x1c, 0x15, 0x0c, 0x19, 0x35, 0x00, 0x06, 0x10, 0x19, 0x18,
  0x04, 0x6e, 0x61, 0x6d, 0x65, 0x15, 0x02, 0x16, 0x04, 0x16, 0x66, 0x16, 0x6e, 0x26, 0xd8, 0x02,
  0x26, 0xa2, 0x02, 0x00, 0x00, 0x26, 0x90, 0x03, 0x1c, 0x15, 0x00, 0x19, 0x25, 0x00, 0x06, 0x19,
  0x18, 0x02, 0x6f, 0x6b, 0x15, 0x02, 0x16, 0x04, 0x16, 0x30, 0x16, 0x34, 0x26, 0x90, 0x03, 0x00,
  0x00, 0x16, 0xdc, 0x01, 0x16, 0x04, 0x00, 0x28, 0x17, 0x66, 0x69, 0x6c, 0x65, 0x5f, 0x73, 0x71,
  0x6c, 0x20, 0x66, 0x69, 0x78, 0x74, 0x75, 0x72, 0x65, 0x20, 0x77, 0x72, 0x69, 0x74, 0x65, 0x72,
  0x00, 0x0f, 0x01, 0x00, 0x00, 0x50, 0x41, 0x52, 0x31,
};
const unsigned char GZIP_V2_PARQUET[] = {
  0x50, 0x41, 0x52, 0x31, 0x15, 0x06, 0x15, 0x34, 0x15, 0x46, 0x5c, 0x15, 0x08, 0x15, 0x02, 0x15,
  0x08, 0x15, 0x00, 0x15, 0x04, 0x15, 0x00, 0x11, 0x00, 0x00, 0x03, 0x0d, 0x1f, 0x8b, 0x08, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x63, 0x60, 0x00, 0x81, 0x07, 0xf6, 0x0c, 0x10, 0x70, 0x00,
  0x42, 0x09, 0x3a, 0x00, 0x00, 0x6b, 0x1d, 0xd3, 0x88, 0x18, 0x00, 0x00, 0x00, 0x15, 0x06, 0x15,
  0x1c, 0x15, 0x3a, 0x5c, 0x15, 0x08, 0x15, 0x02, 0x15, 0x08, 0x15, 0x00, 0x15, 0x04, 0x15, 0x00,
  0x11, 0x00, 0x00, 0x03, 0x0b, 0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0xe3,
  0xf6, 0x65, 0x00, 0x83, 0xff, 0x40, 0x00, 0x00, 0x6a, 0x16, 0x20, 0xc9, 0x0c, 0x00, 0x00, 0x00,
  0x15, 0x02, 0x19, 0x3c, 0x48, 0x06, 0x73, 0x63, 0x68, 0x65, 0x6d, 0x61, 0x15, 0x04, 0x00, 0x15,
  0x0a, 0x25, 0x02, 0x18, 0x01, 0x76, 0x00, 0x15, 0x02, 0x25, 0x02, 0x18, 0x03, 0x64, 0x61, 0x79,
  0x25, 0x0c, 0x4c, 0x6c, 0x00, 0x00, 0x00, 0x16, 0x08, 0x19, 0x1c, 0x19, 0x2c, 0x26, 0x08, 0x1c,
  0x15, 0x0a, 0x19, 0x25, 0x00, 0x06, 0x19, 0x18, 0x01, 0x76, 0x15, 0x04, 0x16, 0x08, 0x16, 0x60,
  0x16, 0x72, 0x26, 0x08, 0x00, 0x00, 0x26, 0x7a, 0x1c, 0x15, 0x02, 0x19, 0x25, 0x00, 0x06, 0x19,
  0x18, 0x03, 0x64, 0x61, 0x79, 0x15, 0x04, 0x16, 0x08, 0x16, 0x48, 0x16, 0x66, 0x26, 0x7a, 0x00,
  0x00, 0x16, 0xd8, 0x01, 0x16, 0x08, 0x00, 0x28, 0x17, 0x66, 0x69, 0x6c, 0x65, 0x5f, 0x73, 0x71,
  0x6c, 0x20, 0x66, 0x69, 0x78, 0x74, 0x75, 0x72, 0x65, 0x20, 0x77, 0x72, 0x69, 0x74, 0x65, 0x72,
  0x00, 0x81, 0x00, 0x00, 0x00, 0x50, 0x41, 0x52, 0x31,
};

class FileScanUtilsTest : public ::testing::Test
{
public:
  FileScanUtilsTest() : alloc_("FileSqlUt") {}
  virtual void SetUp() override
  {
    snprintf(dir_, sizeof(dir_), "/tmp/test_file_scan_utils.%d", static_cast<int>(getpid()));
    ::mkdir(dir_, 0755);
  }
  virtual void TearDown() override
  {
    for (int64_t i = 0; i < files_.count(); ++i) {
      ::unlink(files_.at(i).ptr());
    }
    ::rmdir(dir_);
    alloc_.reset();
  }
protected:
  // Write a fixture file and return its path (NUL terminated, owned by alloc_).
  ObString write_file(const char *name, const char *data, const int64_t len)
  {
    char path[512];
    const int n = snprintf(path, sizeof(path), "%s/%s", dir_, name);
    ObString copied;
    EXPECT_EQ(OB_SUCCESS, ob_write_string(alloc_, ObString(n, path), copied, true));
    const int fd = ::open(copied.ptr(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    EXPECT_LE(0, fd);
    if (fd >= 0) {
      EXPECT_EQ(len, ::write(fd, data, len));
      ::close(fd);
    }
    EXPECT_EQ(OB_SUCCESS, files_.push_back(copied));
    return copied;
  }
  ObString write_file(const char *name, const char *text)
  {
    return write_file(name, text, static_cast<int64_t>(strlen(text)));
  }
  void append_file(const ObString &path, const char *text)
  {
    const int fd = ::open(path.ptr(), O_WRONLY | O_APPEND);
    ASSERT_LE(0, fd);
    ASSERT_EQ(static_cast<ssize_t>(strlen(text)), ::write(fd, text, strlen(text)));
    ::close(fd);
  }
  // Fill path and identity the way the resolver does before inference.
  int prepare(const FileFormat format, const ObString &path, FileTableDesc &desc)
  {
    int ret = OB_SUCCESS;
    LocalFileReader file;
    desc.set_format(format);
    desc.path_ = path;
    desc.real_path_ = path;
    if (OB_SUCC(file.open(path, NULL))) {
      desc.identity_ = file.get_identity();
      file.close();
    }
    return ret;
  }
  int infer(const FileFormat format, const ObString &path, FileTableDesc &desc)
  {
    int ret = prepare(format, path, desc);
    if (OB_FAIL(ret)) {
    } else if (FileFormat::CSV == format) {
      ret = FileSchemaInferrer::infer_csv(alloc_, desc);
    } else if (FileFormat::JSON == format) {
      ret = FileSchemaInferrer::infer_json(alloc_, desc);
    } else {
      ret = infer_parquet_schema(alloc_, desc);
    }
    return ret;
  }
  // Read every row of the projected columns; VARCHAR values stay valid in alloc_.
  int scan(IFileRowSource &source, const FileTableDesc &desc,
           ObIArray<ObSEArray<FileValue, 8>> &rows)
  {
    int ret = OB_SUCCESS;
    ObSEArray<int64_t, 8> idxs;
    for (int64_t i = 0; OB_SUCC(ret) && i < desc.get_column_count(); ++i) {
      ret = idxs.push_back(i);
    }
    FileValue values[16];
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(source.open(desc, idxs, ObString("/")))) {
    } else {
      while (OB_SUCC(ret)) {
        ObSEArray<FileValue, 8> row;
        if (OB_FAIL(source.next_row(alloc_, values))) {
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < idxs.count(); ++i) {
            ret = row.push_back(values[i]);
          }
          if (OB_SUCC(ret)) {
            ret = rows.push_back(row);
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
      source.close();
    }
    return ret;
  }
  static ObString str(const char *s) { return ObString(static_cast<int32_t>(strlen(s)), s); }
protected:
  ObArenaAllocator alloc_;
  char dir_[256];
  ObSEArray<ObString, 16> files_;
};

TEST_F(FileScanUtilsTest, classify_and_widen)
{
  int16_t scale = 0;
  EXPECT_EQ(FileColumnType::BIGINT, FileSqlUtil::classify_text("9223372036854775807", 19, scale));
  EXPECT_EQ(FileColumnType::BIGINT, FileSqlUtil::classify_text("-9223372036854775808", 20, scale));
  EXPECT_EQ(FileColumnType::DOUBLE, FileSqlUtil::classify_text("9223372036854775808", 19, scale));
  EXPECT_EQ(FileColumnType::DOUBLE, FileSqlUtil::classify_text("1e3", 3, scale));
  EXPECT_EQ(FileColumnType::DOUBLE, FileSqlUtil::classify_text("-2.5", 4, scale));
  EXPECT_EQ(FileColumnType::BOOLEAN, FileSqlUtil::classify_text("true", 4, scale));
  EXPECT_EQ(FileColumnType::BOOLEAN, FileSqlUtil::classify_text("FALSE", 5, scale));
  EXPECT_EQ(FileColumnType::DATE, FileSqlUtil::classify_text("2024-02-29", 10, scale));
  EXPECT_EQ(FileColumnType::VARCHAR, FileSqlUtil::classify_text("2026-02-29", 10, scale));
  EXPECT_EQ(FileColumnType::VARCHAR, FileSqlUtil::classify_text("2026-13-01", 10, scale));
  EXPECT_EQ(FileColumnType::DATETIME, FileSqlUtil::classify_text("2026-08-01 10:11:12", 19, scale));
  EXPECT_EQ(0, scale);
  EXPECT_EQ(FileColumnType::DATETIME, FileSqlUtil::classify_text("2026-08-01T10:11:12.125", 23, scale));
  EXPECT_EQ(3, scale);
  EXPECT_EQ(FileColumnType::VARCHAR, FileSqlUtil::classify_text("abc", 3, scale));

  EXPECT_EQ(FileColumnType::BIGINT, widen_file_column_type(FileColumnType::NULL_TYPE, FileColumnType::BIGINT));
  EXPECT_EQ(FileColumnType::DOUBLE, widen_file_column_type(FileColumnType::BIGINT, FileColumnType::DOUBLE));
  EXPECT_EQ(FileColumnType::DATETIME, widen_file_column_type(FileColumnType::DATE, FileColumnType::DATETIME));
  EXPECT_EQ(FileColumnType::VARCHAR, widen_file_column_type(FileColumnType::BOOLEAN, FileColumnType::BIGINT));
  EXPECT_EQ(FileColumnType::VARCHAR, widen_file_column_type(FileColumnType::DATE, FileColumnType::DOUBLE));
  EXPECT_EQ(FileColumnType::BOOLEAN, widen_file_column_type(FileColumnType::BOOLEAN, FileColumnType::NULL_TYPE));

  int64_t days = 0;
  ASSERT_TRUE(FileSqlUtil::parse_date("1970-01-02", 10, days));
  EXPECT_EQ(1, days);
  ASSERT_TRUE(FileSqlUtil::parse_date("1969-12-31", 10, days));
  EXPECT_EQ(-1, days);
  EXPECT_FALSE(FileSqlUtil::parse_date("2023-02-29", 10, days));
  int64_t usec = 0;
  ASSERT_TRUE(FileSqlUtil::parse_datetime("1970-01-01 00:00:01.5", 21, usec, scale));
  EXPECT_EQ(1500000, usec);
  EXPECT_TRUE(FileSqlUtil::is_valid_utf8("\xe4\xb8\xad", 3));
  EXPECT_FALSE(FileSqlUtil::is_valid_utf8("\xff\xfe", 2));
  EXPECT_EQ(2, FileSqlUtil::utf8_char_count("\xe4\xb8\xad" "a", 4));
}

TEST_F(FileScanUtilsTest, csv_inference)
{
  // CRLF records, a BOM, a quoted newline, doubled quotes and empty fields
  const char *text = "\xef\xbb\xbfid,name,score,ok,day,ts,empty,id\r\n"
                     "1,\"a,\r\nb\",1.5,true,2026-08-01,2026-08-01 10:00:00,,7\r\n"
                     "2,\"say \"\"hi\"\"\",2,false,,2026-08-02 11:00:00.25,,8\r\n";
  FileTableDesc desc(alloc_);
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::CSV, write_file("t.csv", text), desc));
  ASSERT_EQ(8, desc.get_column_count());
  EXPECT_TRUE(desc.has_header_);
  EXPECT_EQ(2, desc.row_count_);
  const FileColumnType types[] = {FileColumnType::BIGINT, FileColumnType::VARCHAR,
                                  FileColumnType::DOUBLE, FileColumnType::BOOLEAN,
                                  FileColumnType::DATE, FileColumnType::DATETIME,
                                  FileColumnType::NULL_TYPE, FileColumnType::BIGINT};
  const char *names[] = {"id", "name", "score", "ok", "day", "ts", "empty", "id__2"};
  for (int64_t i = 0; i < desc.get_column_count(); ++i) {
    const FileColumnDesc &col = desc.columns_.at(i);
    EXPECT_EQ(types[i], col.get_type()) << i;
    EXPECT_EQ(0, col.name_.case_compare(names[i])) << i;
  }
  EXPECT_FALSE(desc.columns_.at(0).nullable_);
  EXPECT_TRUE(desc.columns_.at(4).nullable_);
  EXPECT_EQ(2, desc.columns_.at(5).scale_);

  CsvRowSource source;
  ObSEArray<ObSEArray<FileValue, 8>, 4> rows;
  ASSERT_EQ(OB_SUCCESS, scan(source, desc, rows));
  ASSERT_EQ(2, rows.count());
  EXPECT_EQ(1, rows.at(0).at(0).int_);
  EXPECT_EQ(0, rows.at(0).at(1).str_.compare("a,\r\nb"));
  EXPECT_EQ(0, rows.at(1).at(1).str_.compare("say \"hi\""));
  EXPECT_DOUBLE_EQ(1.5, rows.at(0).at(2).double_);
  EXPECT_EQ(1, rows.at(0).at(3).int_);
  EXPECT_TRUE(rows.at(1).at(4).is_null_);
  EXPECT_TRUE(rows.at(0).at(6).is_null_);
  EXPECT_EQ(8, rows.at(1).at(7).int_);
}

TEST_F(FileScanUtilsTest, csv_options)
{
  FileTableDesc desc(alloc_);
  desc.header_option_ = 0;
  desc.delimiter_ = ';';
  desc.quote_ = '\'';
  desc.escape_ = '\\';
  desc.null_str_ = str("NA");
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::CSV, write_file("o.csv", "1;'x;y';NA\n2;'it\\'s';3\n"), desc));
  ASSERT_EQ(3, desc.get_column_count());
  EXPECT_FALSE(desc.has_header_);
  EXPECT_EQ(0, desc.columns_.at(0).name_.compare("column0"));
  EXPECT_EQ(FileColumnType::BIGINT, desc.columns_.at(2).get_type());
  CsvRowSource source;
  ObSEArray<ObSEArray<FileValue, 8>, 4> rows;
  ASSERT_EQ(OB_SUCCESS, scan(source, desc, rows));
  ASSERT_EQ(2, rows.count());
  EXPECT_EQ(0, rows.at(0).at(1).str_.compare("x;y"));
  EXPECT_EQ(0, rows.at(1).at(1).str_.compare("it's"));
  EXPECT_TRUE(rows.at(0).at(2).is_null_);
  EXPECT_EQ(3, rows.at(1).at(2).int_);

  // a delimiter is detected when none is given and the first record has no comma
  FileTableDesc sniffed(alloc_);
  sniffed.delimiter_ = 0;
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::CSV, write_file("p.csv", "a|b\n1|x,y\n"), sniffed));
  EXPECT_EQ('|', sniffed.delimiter_);
  EXPECT_EQ(2, sniffed.get_column_count());
  FileTableDesc comma(alloc_);
  comma.delimiter_ = 0;
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::CSV, write_file("c.csv", "a,b;c\n1,2;3\n"), comma));
  EXPECT_EQ(',', comma.delimiter_);

  // skip leading lines, all_varchar
  FileTableDesc desc2(alloc_);
  desc2.skip_rows_ = 1;
  desc2.header_option_ = 1;
  desc2.all_varchar_ = true;
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::CSV, write_file("s.csv", "junk\na,b\n1,2\n"), desc2));
  ASSERT_EQ(2, desc2.get_column_count());
  EXPECT_EQ(FileColumnType::VARCHAR, desc2.columns_.at(0).get_type());
  EXPECT_EQ(1, desc2.row_count_);
}

TEST_F(FileScanUtilsTest, csv_bad_rows)
{
  const ObString path = write_file("bad.csv", "a,b\n1,2\n3,4,5\n6,7\n");
  FileTableDesc desc(alloc_);
  EXPECT_EQ(OB_ERR_WRONG_VALUE_COUNT_ON_ROW, infer(FileFormat::CSV, path, desc));
  FileTableDesc desc2(alloc_);
  desc2.ignore_errors_ = true;
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::CSV, path, desc2));
  EXPECT_EQ(2, desc2.row_count_);
  EXPECT_EQ(1, desc2.rejected_rows_);
  CsvRowSource source;
  ObSEArray<ObSEArray<FileValue, 8>, 4> rows;
  ASSERT_EQ(OB_SUCCESS, scan(source, desc2, rows));
  ASSERT_EQ(2, rows.count());
  EXPECT_EQ(6, rows.at(1).at(0).int_);

  FileTableDesc desc3(alloc_);
  EXPECT_EQ(OB_ERR_INVALID_CHARACTER_STRING,
            infer(FileFormat::CSV, write_file("utf8.csv", "a\n\xff\n"), desc3));
  FileTableDesc desc4(alloc_);
  desc4.max_line_size_ = 8;
  EXPECT_EQ(OB_ERR_TOO_BIG_ROWSIZE,
            infer(FileFormat::CSV, write_file("long.csv", "a\n0123456789abcdef\n"), desc4));
}

TEST_F(FileScanUtilsTest, json_field_union)
{
  const char *text = "{\"id\":1,\"name\":\"a\",\"v\":1}\n"
                     "\n"
                     "{\"id\":2,\"v\":2.5,\"day\":\"2026-08-02\"}\n"
                     "{\"id\":3,\"name\":null,\"v\":\"x\",\"none\":null}\n";
  FileTableDesc desc(alloc_);
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::JSON, write_file("u.jsonl", text), desc));
  ASSERT_EQ(5, desc.get_column_count());
  const char *names[] = {"id", "name", "v", "day", "none"};
  const FileColumnType types[] = {FileColumnType::BIGINT, FileColumnType::VARCHAR,
                                  FileColumnType::VARCHAR, FileColumnType::DATE,
                                  FileColumnType::NULL_TYPE};
  for (int64_t i = 0; i < desc.get_column_count(); ++i) {
    EXPECT_EQ(0, desc.columns_.at(i).name_.compare(names[i])) << i;
    EXPECT_EQ(types[i], desc.columns_.at(i).get_type()) << i;
  }
  EXPECT_FALSE(desc.columns_.at(0).nullable_);
  EXPECT_TRUE(desc.columns_.at(3).nullable_);
  JsonRowSource source;
  ObSEArray<ObSEArray<FileValue, 8>, 4> rows;
  ASSERT_EQ(OB_SUCCESS, scan(source, desc, rows));
  ASSERT_EQ(3, rows.count());
  EXPECT_TRUE(rows.at(1).at(1).is_null_);
  EXPECT_EQ(0, rows.at(1).at(2).str_.compare("2.5"));
  EXPECT_FALSE(rows.at(1).at(3).is_null_);
  EXPECT_TRUE(rows.at(0).at(3).is_null_);

  FileTableDesc nested(alloc_);
  EXPECT_EQ(OB_NOT_SUPPORTED,
            infer(FileFormat::JSON, write_file("n.jsonl", "{\"a\":{\"b\":1}}\n"), nested));
  FileTableDesc invalid(alloc_);
  EXPECT_EQ(OB_ERR_INVALID_JSON_TEXT,
            infer(FileFormat::JSON, write_file("i.jsonl", "{\"a\":1}\n{\"a\":}\n"), invalid));
  FileTableDesc skipped(alloc_);
  skipped.ignore_errors_ = true;
  ASSERT_EQ(OB_SUCCESS,
            infer(FileFormat::JSON, write_file("s.jsonl", "{\"a\":1}\n{\"a\":}\n{\"a\":3}\n"), skipped));
  EXPECT_EQ(2, skipped.row_count_);
}

TEST_F(FileScanUtilsTest, file_change_detection)
{
  const ObString path = write_file("c.csv", "a\n1\n2\n");
  FileTableDesc desc(alloc_);
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::CSV, path, desc));

  // the file changes between inference and scan
  append_file(path, "3\n");
  {
    CsvRowSource source;
    ObSEArray<ObSEArray<FileValue, 8>, 4> rows;
    EXPECT_EQ(OB_TABLE_DEFINITION_CHANGED, scan(source, desc, rows));
  }

  // the file changes while it is scanned
  FileTableDesc desc2(alloc_);
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::CSV, path, desc2));
  CsvRowSource source;
  ObSEArray<int64_t, 1> idxs;
  ASSERT_EQ(OB_SUCCESS, idxs.push_back(0));
  FileValue value;
  ASSERT_EQ(OB_SUCCESS, source.open(desc2, idxs, ObString("/")));
  ASSERT_EQ(OB_SUCCESS, source.next_row(alloc_, &value));
  EXPECT_EQ(1, value.int_);
  append_file(path, "4\n");
  int ret = OB_SUCCESS;
  while (OB_SUCCESS == (ret = source.next_row(alloc_, &value))) {
  }
  EXPECT_EQ(OB_TABLE_DEFINITION_CHANGED, ret);
  source.close();

  // a rewind after the change does not hide it either
  LocalFileReader file;
  EXPECT_EQ(OB_TABLE_DEFINITION_CHANGED, file.open(path, &desc2.identity_));
}

TEST_F(FileScanUtilsTest, secure_paths)
{
  const ObString path = write_file("p.csv", "a\n1\n");
  ObString real_path;
  EXPECT_EQ(OB_SUCCESS, FileSqlUtil::resolve_input_path(path, ObString(dir_), alloc_, real_path));
  EXPECT_EQ(OB_ERR_NO_PRIVILEGE, FileSqlUtil::resolve_input_path(path, str("/usr"), alloc_, real_path));
  EXPECT_EQ(OB_ERR_NO_PRIVILEGE, FileSqlUtil::resolve_input_path(path, ObString(), alloc_, real_path));
  // ".." may not leave secure_file_priv, whether or not the target exists
  char escaped[600];
  snprintf(escaped, sizeof(escaped), "%s/../no_such_file_sql_input.csv", dir_);
  EXPECT_EQ(OB_ERR_NO_PRIVILEGE,
            FileSqlUtil::resolve_input_path(str(escaped), ObString(dir_), alloc_, real_path));
  snprintf(escaped, sizeof(escaped), "%s/../../etc/hosts", dir_);
  EXPECT_EQ(OB_ERR_NO_PRIVILEGE,
            FileSqlUtil::resolve_input_path(str(escaped), ObString(dir_), alloc_, real_path));
  // a symbolic link to a file outside secure_file_priv
  char link[600];
  snprintf(link, sizeof(link), "%s/link.csv", dir_);
  ObString link_path;
  ASSERT_EQ(OB_SUCCESS, ob_write_string(alloc_, str(link), link_path, true));
  ASSERT_EQ(OB_SUCCESS, files_.push_back(link_path));
  ASSERT_EQ(0, ::symlink("/etc/hosts", link));
  EXPECT_EQ(OB_ERR_NO_PRIVILEGE,
            FileSqlUtil::resolve_input_path(link_path, ObString(dir_), alloc_, real_path));
  EXPECT_EQ(OB_FILE_NOT_EXIST,
            FileSqlUtil::resolve_input_path(str("/tmp/no_such_file_sql_input.csv"), str("/"),
                                            alloc_, real_path));
  // secure_file_priv is resolved first, a real path must lie below it at a directory boundary
  char real_dir[PATH_MAX + 1];
  ASSERT_TRUE(NULL != ::realpath(dir_, real_dir));
  char checked[PATH_MAX + 32];
  snprintf(checked, sizeof(checked), "%s/x.csv", real_dir);
  EXPECT_TRUE(FileSqlUtil::is_path_allowed(ObString(dir_), str(checked)));
  snprintf(checked, sizeof(checked), "%s2/x.csv", real_dir);
  EXPECT_FALSE(FileSqlUtil::is_path_allowed(ObString(dir_), str(checked)));
  // a secure_file_priv that is not an existing directory allows nothing
  EXPECT_FALSE(FileSqlUtil::is_path_allowed(str("/no_such_file_sql_dir"),
                                            str("/no_such_file_sql_dir/x.csv")));
  EXPECT_EQ(OB_NOT_SUPPORTED, FileSqlUtil::check_extension(FileFormat::CSV, str("/a/b.txt")));
  EXPECT_EQ(OB_SUCCESS, FileSqlUtil::check_extension(FileFormat::JSON, str("/a/b.NDJSON")));
}

TEST_F(FileScanUtilsTest, csv_export)
{
  CsvExportOptions options;
  options.header_ = true;
  options.delimiter_ = '|';
  options.null_str_ = str("N/A");
  ASSERT_EQ(OB_SUCCESS, options.column_names_.push_back(str("id")));
  ASSERT_EQ(OB_SUCCESS, options.column_names_.push_back(str("note")));
  ObString encoded;
  ASSERT_EQ(OB_SUCCESS, options.encode(alloc_, encoded));
  EXPECT_TRUE(CsvExportOptions::is_copy_properties(encoded));
  CsvExportOptions decoded;
  ASSERT_EQ(OB_SUCCESS, decoded.decode(encoded));
  EXPECT_EQ('|', decoded.delimiter_);
  EXPECT_TRUE(decoded.header_);

  char target[512];
  snprintf(target, sizeof(target), "%s/out.csv", dir_);
  ObString target_path;
  ASSERT_EQ(OB_SUCCESS, ob_write_string(alloc_, str(target), target_path, true));
  ASSERT_EQ(OB_SUCCESS, files_.push_back(target_path));
  {
    CsvFileExporter exporter;
    ASSERT_EQ(OB_SUCCESS, exporter.open(target_path, decoded));
    ASSERT_EQ(OB_SUCCESS, exporter.write_header());
    ASSERT_EQ(OB_SUCCESS, exporter.write_field("1", 1, false, true));
    ASSERT_EQ(OB_SUCCESS, exporter.write_field("a|\"b\"", 5, false, false));
    ASSERT_EQ(OB_SUCCESS, exporter.end_row());
    ASSERT_EQ(OB_SUCCESS, exporter.write_field(NULL, 0, true, true));
    ASSERT_EQ(OB_SUCCESS, exporter.write_field("", 0, false, false));
    ASSERT_EQ(OB_SUCCESS, exporter.end_row());
    ASSERT_EQ(OB_SUCCESS, exporter.finish());
  }
  char buf[256];
  const int fd = ::open(target, O_RDONLY);
  ASSERT_LE(0, fd);
  const ssize_t n = ::read(fd, buf, sizeof(buf));
  ::close(fd);
  ASSERT_LT(0, n);
  // an empty string is quoted, so it does not read back as NULL
  EXPECT_EQ(0, ObString(static_cast<int32_t>(n), buf).compare("id|note\n1|\"a|\"\"b\"\"\"\nN/A|\"\"\n"));
  {
    // an existing target is never replaced, an aborted export leaves no file behind
    CsvFileExporter exporter;
    EXPECT_EQ(OB_FILE_ALREADY_EXIST, exporter.open(target_path, decoded));
  }
  char target2[512];
  snprintf(target2, sizeof(target2), "%s/aborted.csv", dir_);
  {
    CsvFileExporter exporter;
    ASSERT_EQ(OB_SUCCESS, exporter.open(str(target2), decoded));
    ASSERT_EQ(OB_SUCCESS, exporter.write_header());
    exporter.abort();
  }
  struct stat st;
  EXPECT_NE(0, ::stat(target2, &st));
}

TEST_F(FileScanUtilsTest, parquet_snappy_and_rle)
{
  // literal "abc" followed by an overlapping copy of 9 bytes at offset 3
  const char compressed[] = {0x0c, 0x08, 'a', 'b', 'c', 0x15, 0x03};
  char out[32];
  int64_t out_len = 0;
  ASSERT_EQ(OB_SUCCESS, ParquetUtil::snappy_uncompress(compressed, sizeof(compressed), out,
                                                       sizeof(out), out_len));
  EXPECT_EQ(0, ObString(static_cast<int32_t>(out_len), out).compare("abcabcabcabc"));
  // an offset before the start of the output and a too small buffer are rejected
  const char bad_offset[] = {0x0c, 0x08, 'a', 'b', 'c', 0x15, 0x09};
  EXPECT_NE(OB_SUCCESS, ParquetUtil::snappy_uncompress(bad_offset, sizeof(bad_offset), out,
                                                       sizeof(out), out_len));
  EXPECT_NE(OB_SUCCESS, ParquetUtil::snappy_uncompress(compressed, sizeof(compressed), out, 4,
                                                       out_len));

  // an RLE run of five 1s, then one bit packed group 0,1,0,0,1,1,0,1
  const uint8_t levels[] = {0x0a, 0x01, 0x03, 0xb2};
  ParquetRleDecoder decoder;
  decoder.init(levels, sizeof(levels), 1);
  const uint32_t expected[] = {1, 1, 1, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1};
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(expected) / sizeof(expected[0])); ++i) {
    uint32_t v = 0;
    ASSERT_EQ(OB_SUCCESS, decoder.next(v)) << i;
    EXPECT_EQ(expected[i], v) << i;
  }
  uint32_t v = 0;
  EXPECT_NE(OB_SUCCESS, decoder.next(v));
}

TEST_F(FileScanUtilsTest, parquet_files)
{
  FileTableDesc desc(alloc_);
  const ObString path = write_file("s.parquet", reinterpret_cast<const char *>(SNAPPY_V1_PARQUET),
                                   sizeof(SNAPPY_V1_PARQUET));
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::PARQUET, path, desc));
  ASSERT_EQ(3, desc.get_column_count());
  EXPECT_EQ(5, desc.row_count_);
  EXPECT_EQ(FileColumnType::BIGINT, desc.columns_.at(0).get_type());
  EXPECT_FALSE(desc.columns_.at(0).nullable_);
  EXPECT_EQ(FileColumnType::VARCHAR, desc.columns_.at(1).get_type());
  EXPECT_EQ(FileColumnType::BOOLEAN, desc.columns_.at(2).get_type());
  {
    ParquetRowSource source;
    ObSEArray<ObSEArray<FileValue, 8>, 8> rows;
    ASSERT_EQ(OB_SUCCESS, scan(source, desc, rows));
    ASSERT_EQ(5, rows.count());
    const char *names[] = {"a", NULL, "bb", "a", "ccc"};
    const int64_t oks[] = {1, 0, -1, 1, 0};
    for (int64_t i = 0; i < rows.count(); ++i) {
      EXPECT_EQ(i + 1, rows.at(i).at(0).int_);
      if (NULL == names[i]) {
        EXPECT_TRUE(rows.at(i).at(1).is_null_);
      } else {
        EXPECT_EQ(0, rows.at(i).at(1).str_.compare(names[i])) << i;
      }
      if (oks[i] < 0) {
        EXPECT_TRUE(rows.at(i).at(2).is_null_);
      } else {
        EXPECT_EQ(oks[i], rows.at(i).at(2).int_) << i;
      }
    }
  }
  {
    // column pruning: only the second column is decoded
    ParquetRowSource source;
    ObSEArray<int64_t, 1> idxs;
    ASSERT_EQ(OB_SUCCESS, idxs.push_back(2));
    ASSERT_EQ(OB_SUCCESS, source.open(desc, idxs, ObString("/")));
    FileValue value;
    int64_t count = 0;
    int ret = OB_SUCCESS;
    while (OB_SUCCESS == (ret = source.next_row(alloc_, &value))) {
      ++count;
    }
    EXPECT_EQ(OB_ITER_END, ret);
    EXPECT_EQ(5, count);
    ASSERT_EQ(OB_SUCCESS, source.rewind());
    EXPECT_EQ(OB_SUCCESS, source.next_row(alloc_, &value));
    EXPECT_EQ(1, value.int_);
    source.close();
  }

  FileTableDesc desc2(alloc_);
  const ObString path2 = write_file("g.parquet", reinterpret_cast<const char *>(GZIP_V2_PARQUET),
                                    sizeof(GZIP_V2_PARQUET));
  ASSERT_EQ(OB_SUCCESS, infer(FileFormat::PARQUET, path2, desc2));
  ASSERT_EQ(2, desc2.get_column_count());
  EXPECT_EQ(FileColumnType::DOUBLE, desc2.columns_.at(0).get_type());
  EXPECT_EQ(FileColumnType::DATE, desc2.columns_.at(1).get_type());
  ParquetRowSource source2;
  ObSEArray<ObSEArray<FileValue, 8>, 8> rows2;
  ASSERT_EQ(OB_SUCCESS, scan(source2, desc2, rows2));
  ASSERT_EQ(4, rows2.count());
  EXPECT_DOUBLE_EQ(0.5, rows2.at(0).at(0).double_);
  EXPECT_TRUE(rows2.at(1).at(0).is_null_);
  EXPECT_DOUBLE_EQ(4.25, rows2.at(3).at(0).double_);
  EXPECT_EQ(19723, rows2.at(0).at(1).int_);
  EXPECT_TRUE(rows2.at(2).at(1).is_null_);
  EXPECT_EQ(-1, rows2.at(3).at(1).int_);

  // a truncated file is reported as corrupt
  FileTableDesc desc3(alloc_);
  const ObString path3 = write_file("t.parquet", reinterpret_cast<const char *>(SNAPPY_V1_PARQUET),
                                    sizeof(SNAPPY_V1_PARQUET) / 2);
  EXPECT_NE(OB_SUCCESS, infer(FileFormat::PARQUET, path3, desc3));
}

} // namespace file_scan_utils_test
} // namespace sql
} // namespace oceanbase
