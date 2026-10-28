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

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

#include "lib/container/ob_array_serialization.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_sort.h"
#include "lib/worker.h"
#include "share/ob_errno.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_utils.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

const char *const FILE_SQL_COPY_PREFIX = "FILE_SQL_COPY:";

OB_SERIALIZE_MEMBER(FileIdentity, dev_, ino_, size_, mtime_ns_);
OB_SERIALIZE_MEMBER(FileColumnDesc, name_, source_name_, type_, nullable_, max_length_, scale_,
                    source_idx_, pq_physical_type_, pq_value_kind_, pq_scale_, pq_type_length_,
                    pq_max_def_level_);
OB_SERIALIZE_MEMBER(FileTableDesc, format_, func_name_, path_, real_path_, identity_, delimiter_,
                    quote_, escape_, header_option_, has_header_, null_str_, skip_rows_,
                    all_varchar_, ignore_errors_, max_line_size_, max_object_size_, row_count_,
                    rejected_rows_, columns_);

const char *file_format_name(const FileFormat format)
{
  const char *name = "unknown";
  switch (format) {
    case FileFormat::CSV: name = "csv"; break;
    case FileFormat::JSON: name = "json"; break;
    case FileFormat::PARQUET: name = "parquet"; break;
    case FileFormat::GLOB: name = "glob"; break;
    default: break;
  }
  return name;
}

const char *file_column_type_name(const FileColumnType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case FileColumnType::NULL_TYPE: name = "NULL"; break;
    case FileColumnType::BOOLEAN: name = "BOOLEAN"; break;
    case FileColumnType::BIGINT: name = "BIGINT"; break;
    case FileColumnType::DOUBLE: name = "DOUBLE"; break;
    case FileColumnType::DATE: name = "DATE"; break;
    case FileColumnType::DATETIME: name = "DATETIME"; break;
    case FileColumnType::VARCHAR: name = "VARCHAR"; break;
    default: break;
  }
  return name;
}

FileColumnType widen_file_column_type(const FileColumnType a, const FileColumnType b)
{
  FileColumnType res = FileColumnType::VARCHAR;
  if (FileColumnType::NULL_TYPE == a) {
    res = b;
  } else if (FileColumnType::NULL_TYPE == b || a == b) {
    res = a;
  } else if ((FileColumnType::BIGINT == a && FileColumnType::DOUBLE == b)
             || (FileColumnType::DOUBLE == a && FileColumnType::BIGINT == b)) {
    res = FileColumnType::DOUBLE;
  } else if ((FileColumnType::DATE == a && FileColumnType::DATETIME == b)
             || (FileColumnType::DATETIME == a && FileColumnType::DATE == b)) {
    res = FileColumnType::DATETIME;
  }
  return res;
}

int FileTableDesc::assign(const FileTableDesc &other, ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  format_ = other.format_;
  identity_ = other.identity_;
  delimiter_ = other.delimiter_;
  quote_ = other.quote_;
  escape_ = other.escape_;
  header_option_ = other.header_option_;
  has_header_ = other.has_header_;
  skip_rows_ = other.skip_rows_;
  all_varchar_ = other.all_varchar_;
  ignore_errors_ = other.ignore_errors_;
  max_line_size_ = other.max_line_size_;
  max_object_size_ = other.max_object_size_;
  row_count_ = other.row_count_;
  rejected_rows_ = other.rejected_rows_;
  // strings are deep copied: a plan keeps its own copy of the descriptor
  if (OB_FAIL(ob_write_string(alloc, other.func_name_, func_name_))) {
  } else if (OB_FAIL(ob_write_string(alloc, other.path_, path_))) {
  } else if (OB_FAIL(ob_write_string(alloc, other.real_path_, real_path_))) {
  } else if (OB_FAIL(ob_write_string(alloc, other.null_str_, null_str_))) {
  } else if (OB_FAIL(columns_.assign(other.columns_))) {
    LOG_WARN("failed to assign columns", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    FileColumnDesc &col = columns_.at(i);
    if (OB_FAIL(ob_write_string(alloc, other.columns_.at(i).name_, col.name_))) {
    } else if (OB_FAIL(ob_write_string(alloc, other.columns_.at(i).source_name_, col.source_name_))) {
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// paths and security
// ---------------------------------------------------------------------------------------

int FileSqlUtil::set_user_error(const int err, const char *fmt, ...)
{
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  FORWARD_USER_ERROR_MSG(err, "%s", msg);
  return err;
}

int FileSqlUtil::canonicalize_path(const ObString &path, const bool must_exist,
                                   ObIAllocator &alloc, ObString &real_path, bool &exists)
{
  int ret = OB_SUCCESS;
  char path_buf[PATH_MAX + 1];
  char real_buf[PATH_MAX + 1];
  exists = false;
  if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: empty path");
  } else if (path.length() >= PATH_MAX) {
    ret = OB_SIZE_OVERFLOW;
    set_user_error(ret, "file sql: path is too long");
  } else if (NULL != memchr(path.ptr(), '\0', path.length())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: path contains NUL");
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    if (NULL != ::realpath(path_buf, real_buf)) {
      exists = true;
    } else if (must_exist || ENOENT != errno) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      // resolve the parent directory and append the last component
      char *slash = strrchr(path_buf, '/');
      const char *base = NULL;
      char parent[PATH_MAX + 1];
      if (NULL == slash) {
        parent[0] = '.';
        parent[1] = '\0';
        base = path_buf;
      } else if (slash == path_buf) {
        parent[0] = '/';
        parent[1] = '\0';
        base = slash + 1;
      } else {
        MEMCPY(parent, path_buf, slash - path_buf);
        parent[slash - path_buf] = '\0';
        base = slash + 1;
      }
      if ('\0' == base[0] || 0 == strcmp(base, ".") || 0 == strcmp(base, "..")) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file sql: path must name a file");
      } else if (NULL == ::realpath(parent, real_buf)) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        const int64_t dir_len = strlen(real_buf);
        const int64_t base_len = strlen(base);
        if (dir_len + 1 + base_len >= PATH_MAX) {
          ret = OB_SIZE_OVERFLOW;
        } else {
          if (dir_len > 1 || real_buf[0] != '/') {
            real_buf[dir_len] = '/';
            MEMCPY(real_buf + dir_len + 1, base, base_len + 1);
          } else {
            MEMCPY(real_buf + dir_len, base, base_len + 1);
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ob_write_string(alloc, ObString(real_buf), real_path, true))) {
        LOG_WARN("failed to copy path", K(ret));
      }
    }
  }
  return ret;
}

bool FileSqlUtil::is_path_allowed(const ObString &secure_file_priv, const ObString &real_path)
{
  bool allowed = false;
  char priv_buf[PATH_MAX + 1];
  char real_priv[PATH_MAX + 1];
  struct stat st;
  if (secure_file_priv.empty() || 0 == secure_file_priv.case_compare("NULL")
      || secure_file_priv.length() >= PATH_MAX || real_path.empty() || '/' != real_path[0]) {
    // denied
  } else {
    MEMCPY(priv_buf, secure_file_priv.ptr(), secure_file_priv.length());
    priv_buf[secure_file_priv.length()] = '\0';
    if (NULL == ::realpath(priv_buf, real_priv) || 0 != ::stat(real_priv, &st)
        || !S_ISDIR(st.st_mode)) {
      // denied
    } else {
      const ObString priv(real_priv);
      allowed = priv == "/"
                || (real_path.prefix_match(priv)
                    && (real_path.length() == priv.length() || real_path[priv.length()] == '/'));
    }
  }
  return allowed;
}

int FileSqlUtil::check_secure_path(const ObString &secure_file_priv, const ObString &real_path)
{
  int ret = OB_SUCCESS;
  if (!is_path_allowed(secure_file_priv, real_path)) {
    ret = OB_ERR_NO_PRIVILEGE;
    LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "FILE (path is outside secure_file_priv)");
  }
  return ret;
}

// Best effort canonical form of a path whose parent directory does not exist: the longest
// existing ancestor is resolved with realpath() and the rest is appended lexically.
static int approximate_real_path(const ObString &path, ObIAllocator &alloc, ObString &out)
{
  int ret = OB_SUCCESS;
  char abs_buf[2 * PATH_MAX + 2];
  char norm[2 * PATH_MAX + 2];
  char real_buf[PATH_MAX + 1];
  int64_t abs_len = 0;
  if (path.empty() || path.length() >= PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
  } else if ('/' != path[0]) {
    if (NULL == ::getcwd(abs_buf, PATH_MAX)) {
      ret = OB_IO_ERROR;
    } else {
      abs_len = strlen(abs_buf);
      abs_buf[abs_len++] = '/';
    }
  }
  if (OB_SUCC(ret)) {
    MEMCPY(abs_buf + abs_len, path.ptr(), path.length());
    abs_len += path.length();
    abs_buf[abs_len] = '\0';
    // lexical normalization: drop empty and '.' components, '..' pops one
    int64_t n = 0;
    int64_t i = 0;
    while (i < abs_len) {
      while (i < abs_len && '/' == abs_buf[i]) {
        ++i;
      }
      const int64_t start = i;
      while (i < abs_len && '/' != abs_buf[i]) {
        ++i;
      }
      const int64_t clen = i - start;
      if (0 == clen || (1 == clen && '.' == abs_buf[start])) {
      } else if (2 == clen && '.' == abs_buf[start] && '.' == abs_buf[start + 1]) {
        while (n > 0 && '/' != norm[n - 1]) {
          --n;
        }
        if (n > 0) {
          --n;
        }
      } else {
        norm[n++] = '/';
        MEMCPY(norm + n, abs_buf + start, clen);
        n += clen;
      }
    }
    if (0 == n) {
      norm[n++] = '/';
    }
    norm[n] = '\0';
    int64_t cut = n;
    bool found = false;
    while (!found && cut > 0) {
      const char saved = norm[cut];
      norm[cut] = '\0';
      if (NULL != ::realpath(norm, real_buf)) {
        found = true;
      }
      norm[cut] = saved;
      if (!found) {
        do {
          --cut;
        } while (cut > 0 && '/' != norm[cut]);
      }
    }
    ObSqlString result;
    if (!found) {
      ret = result.append(norm);
    } else if (0 == strcmp(real_buf, "/")) {
      ret = result.append(cut < n ? norm + cut : "/");
    } else {
      ret = result.append_fmt("%s%s", real_buf, norm + cut);
    }
    if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc, result.string(), out, true))) {
    }
  }
  return ret;
}

int FileSqlUtil::resolve_input_path(const ObString &path, const ObString &secure_file_priv,
                                    ObIAllocator &alloc, ObString &real_path)
{
  int ret = OB_SUCCESS;
  bool exists = false;
  if (OB_FAIL(canonicalize_path(path, false, alloc, real_path, exists))) {
    if (OB_FILE_NOT_EXIST == ret) {
      ObString approx;
      if (OB_SUCCESS == approximate_real_path(path, alloc, approx)
          && !is_path_allowed(secure_file_priv, approx)) {
        ret = check_secure_path(secure_file_priv, approx);
      } else {
        ret = set_user_error(OB_FILE_NOT_EXIST, "File not exist: '%.*s'", path.length(), path.ptr());
      }
    }
  } else if (OB_FAIL(check_secure_path(secure_file_priv, real_path))) {
  } else if (!exists) {
    ret = set_user_error(OB_FILE_NOT_EXIST, "File not exist: '%.*s'", path.length(), path.ptr());
  }
  return ret;
}

int FileSqlUtil::resolve_output_path(const ObString &path, const ObString &secure_file_priv,
                                     ObIAllocator &alloc, ObString &real_path)
{
  int ret = OB_SUCCESS;
  bool exists = false;
  struct stat st;
  if (OB_FAIL(canonicalize_path(path, false, alloc, real_path, exists))) {
    if (OB_FILE_NOT_EXIST == ret) {
      ObString approx;
      if (OB_SUCCESS == approximate_real_path(path, alloc, approx)
          && !is_path_allowed(secure_file_priv, approx)) {
        ret = check_secure_path(secure_file_priv, approx);
      } else {
        ret = set_user_error(OB_FILE_NOT_EXIST, "COPY: directory of '%.*s' does not exist",
                             path.length(), path.ptr());
      }
    }
  } else if (OB_FAIL(check_secure_path(secure_file_priv, real_path))) {
  } else if (exists || 0 == ::lstat(real_path.ptr(), &st)) {
    ret = set_user_error(OB_FILE_ALREADY_EXIST, "COPY: target file '%.*s' already exists",
                         path.length(), path.ptr());
  }
  return ret;
}

int FileSqlUtil::get_fd_identity(const int fd, FileIdentity &identity)
{
  int ret = OB_SUCCESS;
  struct stat st;
  if (0 != ::fstat(fd, &st)) {
    ret = OB_IO_ERROR;
    LOG_WARN("fstat failed", K(ret), K(errno));
  } else {
    identity.dev_ = static_cast<int64_t>(st.st_dev);
    identity.ino_ = static_cast<int64_t>(st.st_ino);
    identity.size_ = static_cast<int64_t>(st.st_size);
#ifdef __APPLE__
    identity.mtime_ns_ = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000L
                         + st.st_mtimespec.tv_nsec;
#else
    identity.mtime_ns_ = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000L
                         + st.st_mtim.tv_nsec;
#endif
  }
  return ret;
}

int FileSqlUtil::open_read_only(const ObString &real_path, int &fd, FileIdentity &identity)
{
  int ret = OB_SUCCESS;
  char path_buf[PATH_MAX + 1];
  struct stat st;
  fd = -1;
  if (real_path.empty() || real_path.length() >= PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(path_buf, real_path.ptr(), real_path.length());
    path_buf[real_path.length()] = '\0';
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_NONBLOCK
    // never block on a fifo; regular files ignore the flag
    flags |= O_NONBLOCK;
#endif
    fd = ::open(path_buf, flags);
    if (fd < 0) {
      ret = (ENOENT == errno || ENOTDIR == errno) ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
      if (OB_IO_ERROR == ret) {
        set_user_error(ret, "file sql: cannot open '%.*s' (errno %d)",
                       real_path.length(), real_path.ptr(), errno);
      }
    } else if (0 != ::fstat(fd, &st)) {
      ret = OB_IO_ERROR;
    } else if (!S_ISREG(st.st_mode)) {
      ret = OB_NOT_SUPPORTED;
      set_user_error(ret, "file sql: '%.*s' is not a regular file, reading it is not supported",
                     real_path.length(), real_path.ptr());
    } else if (OB_FAIL(get_fd_identity(fd, identity))) {
    }
    if (OB_FAIL(ret) && fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  return ret;
}

int FileSqlUtil::check_extension(const FileFormat format, const ObString &path)
{
  int ret = OB_SUCCESS;
  const char *dot = NULL;
  for (int64_t i = path.length() - 1; i >= 0 && NULL == dot; --i) {
    if ('.' == path[i]) {
      dot = path.ptr() + i;
    } else if ('/' == path[i]) {
      break;
    }
  }
  ObString ext;
  if (NULL != dot) {
    ext.assign_ptr(dot + 1, static_cast<int32_t>(path.ptr() + path.length() - dot - 1));
  }
  bool ok = false;
  if (FileFormat::CSV == format) {
    ok = 0 == ext.case_compare("csv") || 0 == ext.case_compare("tsv")
         || 0 == ext.case_compare("tab");
  } else if (FileFormat::JSON == format) {
    ok = 0 == ext.case_compare("json") || 0 == ext.case_compare("jsonl")
         || 0 == ext.case_compare("ndjson");
  } else if (FileFormat::PARQUET == format) {
    ok = 0 == ext.case_compare("parquet") || 0 == ext.case_compare("parq");
  } else {
    ok = true;
  }
  if (!ok) {
    ret = OB_NOT_SUPPORTED;
    set_user_error(ret, "file sql: extension '.%.*s' is not supported by the %s reader",
                   ext.length(), ext.ptr(), file_format_name(format));
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// text helpers
// ---------------------------------------------------------------------------------------

bool FileSqlUtil::is_valid_utf8(const char *ptr, const int64_t len)
{
  bool valid = true;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(ptr);
  int64_t i = 0;
  while (valid && i < len) {
    const unsigned char c = p[i];
    if (c < 0x80) {
      ++i;
    } else if (c >= 0xC2 && c <= 0xDF) {
      valid = i + 1 < len && (p[i + 1] & 0xC0) == 0x80;
      i += 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
      valid = i + 2 < len && (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80;
      if (valid) {
        const uint32_t cp = ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
        valid = cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF);
      }
      i += 3;
    } else if (c >= 0xF0 && c <= 0xF4) {
      valid = i + 3 < len && (p[i + 1] & 0xC0) == 0x80 && (p[i + 2] & 0xC0) == 0x80
              && (p[i + 3] & 0xC0) == 0x80;
      if (valid) {
        const uint32_t cp = ((c & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12)
                            | ((p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
        valid = cp >= 0x10000 && cp <= 0x10FFFF;
      }
      i += 4;
    } else {
      valid = false;
    }
  }
  return valid;
}

int64_t FileSqlUtil::utf8_char_count(const char *ptr, const int64_t len)
{
  int64_t count = 0;
  for (int64_t i = 0; i < len; ++i) {
    if ((static_cast<unsigned char>(ptr[i]) & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

static inline bool is_blank(const char c)
{
  return ' ' == c || '\t' == c;
}

static inline void trim_blank(const char *&ptr, int64_t &len)
{
  while (len > 0 && is_blank(ptr[0])) {
    ++ptr;
    --len;
  }
  while (len > 0 && is_blank(ptr[len - 1])) {
    --len;
  }
}

static inline bool is_digit(const char c)
{
  return c >= '0' && c <= '9';
}

bool FileSqlUtil::parse_bool(const char *ptr, const int64_t len, bool &value)
{
  bool ok = false;
  const char *p = ptr;
  int64_t l = len;
  trim_blank(p, l);
  if (4 == l && 0 == strncasecmp(p, "true", 4)) {
    value = true;
    ok = true;
  } else if (5 == l && 0 == strncasecmp(p, "false", 5)) {
    value = false;
    ok = true;
  }
  return ok;
}

bool FileSqlUtil::parse_bigint(const char *ptr, const int64_t len, int64_t &value)
{
  bool ok = false;
  const char *p = ptr;
  int64_t l = len;
  trim_blank(p, l);
  if (l > 0) {
    int64_t i = 0;
    bool neg = false;
    if ('+' == p[0] || '-' == p[0]) {
      neg = '-' == p[0];
      i = 1;
    }
    if (i < l) {
      uint64_t acc = 0;
      const uint64_t limit = neg ? static_cast<uint64_t>(INT64_MAX) + 1 : static_cast<uint64_t>(INT64_MAX);
      ok = true;
      for (; ok && i < l; ++i) {
        if (!is_digit(p[i])) {
          ok = false;
        } else {
          const uint64_t d = static_cast<uint64_t>(p[i] - '0');
          if (acc > (limit - d) / 10) {
            ok = false;
          } else {
            acc = acc * 10 + d;
          }
        }
      }
      if (ok) {
        value = neg ? static_cast<int64_t>(0 - acc) : static_cast<int64_t>(acc);
      }
    }
  }
  return ok;
}

bool FileSqlUtil::parse_double(const char *ptr, const int64_t len, double &value)
{
  bool ok = false;
  const char *p = ptr;
  int64_t l = len;
  trim_blank(p, l);
  // [sign] (digits [. digits*] | . digits) [(e|E) [sign] digits]
  if (l > 0 && l < 512) {
    int64_t i = 0;
    if ('+' == p[i] || '-' == p[i]) {
      ++i;
    }
    int64_t int_digits = 0;
    int64_t frac_digits = 0;
    while (i < l && is_digit(p[i])) {
      ++i;
      ++int_digits;
    }
    if (i < l && '.' == p[i]) {
      ++i;
      while (i < l && is_digit(p[i])) {
        ++i;
        ++frac_digits;
      }
    }
    bool valid = (int_digits + frac_digits) > 0;
    if (valid && i < l && ('e' == p[i] || 'E' == p[i])) {
      ++i;
      if (i < l && ('+' == p[i] || '-' == p[i])) {
        ++i;
      }
      int64_t exp_digits = 0;
      while (i < l && is_digit(p[i])) {
        ++i;
        ++exp_digits;
      }
      valid = exp_digits > 0;
    }
    if (valid && i == l) {
      char buf[512];
      MEMCPY(buf, p, l);
      buf[l] = '\0';
      char *end = NULL;
      errno = 0;
      const double v = strtod(buf, &end);
      if (end == buf + l && 0 != isfinite(v) && !(ERANGE == errno && fabs(v) > 1.0)) {
        value = v;
        ok = true;
      }
    }
  }
  return ok;
}

bool FileSqlUtil::is_valid_date(const int64_t y, const int64_t m, const int64_t d)
{
  static const int64_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool valid = y >= 1 && y <= 9999 && m >= 1 && m <= 12 && d >= 1;
  if (valid) {
    int64_t max_day = DAYS[m - 1];
    if (2 == m && ((0 == y % 4 && 0 != y % 100) || 0 == y % 400)) {
      max_day = 29;
    }
    valid = d <= max_day;
  }
  return valid;
}

int64_t FileSqlUtil::days_from_civil(int64_t y, const int64_t m, const int64_t d)
{
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static inline bool parse_fixed_digits(const char *p, const int64_t n, int64_t &v)
{
  bool ok = true;
  v = 0;
  for (int64_t i = 0; ok && i < n; ++i) {
    ok = is_digit(p[i]);
    v = v * 10 + (p[i] - '0');
  }
  return ok;
}

bool FileSqlUtil::parse_date(const char *ptr, const int64_t len, int64_t &days)
{
  bool ok = false;
  const char *p = ptr;
  int64_t l = len;
  int64_t y = 0;
  int64_t m = 0;
  int64_t d = 0;
  trim_blank(p, l);
  if (10 == l && '-' == p[4] && '-' == p[7] && parse_fixed_digits(p, 4, y)
      && parse_fixed_digits(p + 5, 2, m) && parse_fixed_digits(p + 8, 2, d)
      && is_valid_date(y, m, d)) {
    days = days_from_civil(y, m, d);
    ok = true;
  }
  return ok;
}

bool FileSqlUtil::parse_datetime(const char *ptr, const int64_t len, int64_t &usec, int16_t &scale)
{
  bool ok = false;
  const char *p = ptr;
  int64_t l = len;
  int64_t days = 0;
  int64_t hh = 0;
  int64_t mi = 0;
  int64_t ss = 0;
  trim_blank(p, l);
  scale = 0;
  // YYYY-MM-DD[ T]HH:MM:SS[.f{1,9}]
  if (l >= 19 && (' ' == p[10] || 'T' == p[10] || 't' == p[10]) && parse_date(p, 10, days)
      && ':' == p[13] && ':' == p[16] && parse_fixed_digits(p + 11, 2, hh)
      && parse_fixed_digits(p + 14, 2, mi) && parse_fixed_digits(p + 17, 2, ss)
      && hh <= 23 && mi <= 59 && ss <= 59) {
    int64_t frac = 0;
    int64_t frac_digits = 0;
    ok = true;
    if (l > 19) {
      if ('.' != p[19] || l == 20 || l > 29) {
        ok = false;
      } else {
        for (int64_t i = 20; ok && i < l; ++i) {
          if (!is_digit(p[i])) {
            ok = false;
          } else if (frac_digits < 6) {
            frac = frac * 10 + (p[i] - '0');
            ++frac_digits;
          }
        }
        scale = static_cast<int16_t>(l - 20 > 6 ? 6 : l - 20);
        for (int64_t i = frac_digits; i < 6; ++i) {
          frac *= 10;
        }
      }
    }
    if (ok) {
      usec = ((days * 24 + hh) * 60 + mi) * 60 * 1000000L + ss * 1000000L + frac;
    }
  }
  return ok;
}

FileColumnType FileSqlUtil::classify_text(const char *ptr, const int64_t len, int16_t &scale)
{
  FileColumnType type = FileColumnType::VARCHAR;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  scale = 0;
  if (len > 0 && len < 64) {
    const char c = ptr[0];
    if ((c >= '0' && c <= '9') || '+' == c || '-' == c || '.' == c || is_blank(c)) {
      if (parse_bigint(ptr, len, i)) {
        type = FileColumnType::BIGINT;
      } else if (parse_double(ptr, len, d)) {
        type = FileColumnType::DOUBLE;
      } else if (parse_date(ptr, len, i)) {
        type = FileColumnType::DATE;
      } else if (parse_datetime(ptr, len, i, scale)) {
        type = FileColumnType::DATETIME;
      }
    }
    if (FileColumnType::VARCHAR == type && parse_bool(ptr, len, b)) {
      type = FileColumnType::BOOLEAN;
    }
  }
  return type;
}

FileColumnType FileSqlUtil::classify_json_string(const char *ptr, const int64_t len, int16_t &scale)
{
  FileColumnType type = FileColumnType::VARCHAR;
  int64_t v = 0;
  scale = 0;
  if (len >= 10 && len <= 29 && is_digit(ptr[0])) {
    if (parse_date(ptr, len, v)) {
      type = FileColumnType::DATE;
    } else if (parse_datetime(ptr, len, v, scale)) {
      type = FileColumnType::DATETIME;
    }
  }
  return type;
}

bool FileSqlUtil::convert_text(const FileColumnType type, const char *ptr, const int64_t len,
                               FileValue &value)
{
  bool ok = true;
  bool b = false;
  int16_t scale = 0;
  value.is_null_ = false;
  switch (type) {
    case FileColumnType::NULL_TYPE:
      value.is_null_ = true;
      break;
    case FileColumnType::BOOLEAN:
      ok = parse_bool(ptr, len, b);
      value.int_ = b ? 1 : 0;
      break;
    case FileColumnType::BIGINT:
      ok = parse_bigint(ptr, len, value.int_);
      break;
    case FileColumnType::DOUBLE:
      ok = parse_double(ptr, len, value.double_);
      break;
    case FileColumnType::DATE:
      ok = parse_date(ptr, len, value.int_);
      break;
    case FileColumnType::DATETIME:
      if (!parse_datetime(ptr, len, value.int_, scale)) {
        int64_t days = 0;
        ok = parse_date(ptr, len, days);
        value.int_ = days * 86400L * 1000000L;
      }
      break;
    case FileColumnType::VARCHAR:
      value.str_.assign_ptr(ptr, static_cast<int32_t>(len));
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

int FileSqlUtil::make_unique_names(ObIAllocator &alloc, FileColumnArray &columns)
{
  int ret = OB_SUCCESS;
  const int64_t count = columns.count();
  int64_t digits = 1;
  for (int64_t n = count - 1; n >= 10; n /= 10) {
    ++digits;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    FileColumnDesc &col = columns.at(i);
    ObString base = col.source_name_;
    char buf[128];
    if (base.empty()) {
      int n = snprintf(buf, sizeof(buf), "column%0*ld", static_cast<int>(digits), i);
      base.assign_ptr(buf, n);
    } else if (utf8_char_count(base.ptr(), base.length()) > FileSqlLimits::MAX_COLUMN_NAME_CHARS) {
      // keep the first MAX_COLUMN_NAME_CHARS characters
      int64_t chars = 0;
      int64_t pos = 0;
      while (pos < base.length()) {
        if ((static_cast<unsigned char>(base[pos]) & 0xC0) != 0x80) {
          if (chars == FileSqlLimits::MAX_COLUMN_NAME_CHARS) {
            break;
          }
          ++chars;
        }
        ++pos;
      }
      base.assign_ptr(base.ptr(), static_cast<int32_t>(pos));
    }
    ObString name;
    for (int64_t k = 1; OB_SUCC(ret) && name.empty(); ++k) {
      ObSqlString candidate;
      if (1 == k) {
        ret = candidate.append(base);
      } else {
        ret = candidate.append_fmt("%.*s__%ld", base.length(), base.ptr(), k);
      }
      bool dup = false;
      for (int64_t j = 0; OB_SUCC(ret) && !dup && j < i; ++j) {
        dup = 0 == columns.at(j).name_.case_compare(candidate.string());
      }
      for (int64_t j = i + 1; OB_SUCC(ret) && !dup && k > 1 && j < count; ++j) {
        // do not take a name that a later column carries in the source
        dup = 0 == columns.at(j).source_name_.case_compare(candidate.string());
      }
      if (OB_SUCC(ret) && !dup) {
        if (OB_FAIL(ob_write_string(alloc, candidate.string(), name))) {
          LOG_WARN("failed to write name", K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      col.name_ = name;
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// LocalFileReader
// ---------------------------------------------------------------------------------------

int LocalFileReader::open(const ObString &real_path, const FileIdentity *expected)
{
  int ret = OB_SUCCESS;
  close();
  path_ = real_path;
  if (OB_FAIL(FileSqlUtil::open_read_only(real_path, fd_, identity_))) {
    if (OB_FILE_NOT_EXIST == ret && NULL != expected && expected->is_valid()) {
      ret = OB_TABLE_DEFINITION_CHANGED;
      FileSqlUtil::set_user_error(ret, "file sql: '%.*s' was removed after its schema was inferred",
                                  real_path.length(), real_path.ptr());
    }
  } else if (NULL != expected && expected->is_valid() && !identity_.equals(*expected)) {
    ret = OB_TABLE_DEFINITION_CHANGED;
    FileSqlUtil::set_user_error(ret, "file sql: '%.*s' changed after its schema was inferred, please retry",
                                real_path.length(), real_path.ptr());
    close();
  } else {
    offset_ = 0;
  }
  return ret;
}

int LocalFileReader::read(char *buf, const int64_t len, int64_t &read_len)
{
  int ret = OB_SUCCESS;
  read_len = 0;
  if (OB_FAIL(pread(buf, len, offset_, read_len))) {
  } else {
    offset_ += read_len;
  }
  return ret;
}

int LocalFileReader::pread(char *buf, const int64_t len, const int64_t offset, int64_t &read_len)
{
  int ret = OB_SUCCESS;
  read_len = 0;
  if (fd_ < 0) {
    ret = OB_NOT_INIT;
  } else if (offset < 0 || len < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // never read beyond the size observed at open
    const int64_t limit = std::min(len, std::max<int64_t>(identity_.size_ - offset, 0));
    while (OB_SUCC(ret) && read_len < limit) {
      const ssize_t n = ::pread(fd_, buf + read_len, limit - read_len, offset + read_len);
      if (n < 0) {
        if (EINTR != errno) {
          ret = OB_IO_ERROR;
          FileSqlUtil::set_user_error(ret, "file sql: read '%.*s' failed (errno %d)",
                                      path_.length(), path_.ptr(), errno);
        }
      } else if (0 == n) {
        // file shrank
        ret = OB_TABLE_DEFINITION_CHANGED;
        FileSqlUtil::set_user_error(ret, "file sql: '%.*s' was truncated during the scan",
                                    path_.length(), path_.ptr());
      } else {
        read_len += n;
      }
    }
  }
  return ret;
}

int LocalFileReader::check_unchanged() const
{
  int ret = OB_SUCCESS;
  FileIdentity now;
  if (fd_ < 0) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(FileSqlUtil::get_fd_identity(fd_, now))) {
  } else if (!now.equals(identity_)) {
    ret = OB_TABLE_DEFINITION_CHANGED;
    FileSqlUtil::set_user_error(ret, "file sql: '%.*s' changed during the scan, please retry",
                                path_.length(), path_.ptr());
  }
  return ret;
}

void LocalFileReader::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

// ---------------------------------------------------------------------------------------
// CsvRecordReader
// ---------------------------------------------------------------------------------------

CsvRecordReader::CsvRecordReader()
  : file_(NULL), delimiter_(','), quote_('"'), escape_('"'),
    max_line_size_(FileSqlLimits::DEFAULT_MAX_LINE_SIZE), buf_(NULL), buf_cap_(0), pos_(0),
    end_(0), eof_(false), bom_checked_(false), line_no_(1)
{}

void CsvRecordReader::destroy()
{
  if (NULL != buf_) {
    ob_free(buf_);
    buf_ = NULL;
  }
  buf_cap_ = 0;
  file_ = NULL;
}

int CsvRecordReader::init(LocalFileReader &file, const FileTableDesc &desc)
{
  int ret = OB_SUCCESS;
  destroy();
  file_ = &file;
  delimiter_ = static_cast<char>(desc.delimiter_);
  quote_ = static_cast<char>(desc.quote_);
  escape_ = static_cast<char>(desc.escape_);
  max_line_size_ = desc.max_line_size_;
  buf_cap_ = 2 * FileSqlLimits::READ_BLOCK_SIZE;
  if (OB_ISNULL(buf_ = static_cast<char *>(ob_malloc(buf_cap_, ObMemAttr("FileSqlCsv"))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to alloc csv buffer", K(ret));
  } else {
    pos_ = 0;
    end_ = 0;
    eof_ = false;
    bom_checked_ = false;
    line_no_ = 1;
  }
  return ret;
}

int CsvRecordReader::rewind()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(file_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(file_->rewind())) {
  } else {
    pos_ = 0;
    end_ = 0;
    eof_ = false;
    bom_checked_ = false;
    line_no_ = 1;
  }
  return ret;
}

// make sure buf_[pos_ + rel] is loaded; has_more is false at end of file
int CsvRecordReader::ensure_data(const int64_t rel, bool &has_more)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && pos_ + rel >= end_ && !eof_) {
    if (end_ == buf_cap_) {
      if (pos_ > 0) {
        MEMMOVE(buf_, buf_ + pos_, end_ - pos_);
        end_ -= pos_;
        pos_ = 0;
      } else {
        const int64_t cap_limit = max_line_size_ + FileSqlLimits::READ_BLOCK_SIZE;
        const int64_t new_cap = buf_cap_ * 2 < cap_limit ? buf_cap_ * 2 : cap_limit;
        char *new_buf = NULL;
        if (new_cap <= buf_cap_) {
          ret = OB_ERR_TOO_BIG_ROWSIZE;
          FileSqlUtil::set_user_error(ret, "read_csv: record starting at line %ld exceeds max_line_size (%ld bytes)",
                                      line_no_, max_line_size_);
        } else if (OB_ISNULL(new_buf = static_cast<char *>(ob_malloc(new_cap, ObMemAttr("FileSqlCsv"))))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(new_buf, buf_, end_);
          ob_free(buf_);
          buf_ = new_buf;
          buf_cap_ = new_cap;
        }
      }
    }
    if (OB_SUCC(ret)) {
      int64_t read_len = 0;
      if (OB_FAIL(file_->read(buf_ + end_, buf_cap_ - end_, read_len))) {
      } else if (0 == read_len) {
        eof_ = true;
      } else {
        end_ += read_len;
      }
    }
  }
  has_more = pos_ + rel < end_;
  return ret;
}

int CsvRecordReader::find_record_end(int64_t &rec_len, int64_t &next_off, int64_t &newlines)
{
  int ret = OB_SUCCESS;
  bool in_quotes = false;
  bool field_start = true;
  bool done = false;
  bool has = false;
  int64_t off = 0;
  newlines = 0;
  while (OB_SUCC(ret) && !done) {
    if (off > max_line_size_) {
      ret = OB_ERR_TOO_BIG_ROWSIZE;
      FileSqlUtil::set_user_error(ret, "read_csv: record starting at line %ld exceeds max_line_size (%ld bytes)",
                                  line_no_, max_line_size_);
    } else if (OB_FAIL(ensure_data(off, has))) {
    } else if (!has) {
      if (in_quotes) {
        ret = OB_ERR_WRONG_VALUE_COUNT_ON_ROW;
        FileSqlUtil::set_user_error(ret, "read_csv: unterminated quoted field in record starting at line %ld",
                                    line_no_);
      } else {
        rec_len = off;
        next_off = off;
        done = true;
      }
    } else {
      const char c = buf_[pos_ + off];
      if (in_quotes) {
        if (0 != escape_ && escape_ != quote_ && c == escape_) {
          if (OB_FAIL(ensure_data(off + 1, has))) {
          } else if (!has) {
            ret = OB_ERR_WRONG_VALUE_COUNT_ON_ROW;
            FileSqlUtil::set_user_error(ret, "read_csv: unterminated quoted field in record starting at line %ld",
                                        line_no_);
          } else {
            newlines += '\n' == buf_[pos_ + off + 1] ? 1 : 0;
            off += 2;
          }
        } else if (c == quote_) {
          if (escape_ == quote_) {
            if (OB_FAIL(ensure_data(off + 1, has))) {
            } else if (has && buf_[pos_ + off + 1] == quote_) {
              off += 2;
            } else {
              in_quotes = false;
              ++off;
            }
          } else {
            in_quotes = false;
            ++off;
          }
        } else {
          newlines += '\n' == c ? 1 : 0;
          ++off;
        }
      } else if ('\n' == c) {
        rec_len = off;
        next_off = off + 1;
        ++newlines;
        done = true;
      } else if ('\r' == c) {
        if (OB_FAIL(ensure_data(off + 1, has))) {
        } else {
          rec_len = off;
          next_off = (has && '\n' == buf_[pos_ + off + 1]) ? off + 2 : off + 1;
          ++newlines;
          done = true;
        }
      } else if (field_start && 0 != quote_ && c == quote_) {
        in_quotes = true;
        field_start = false;
        ++off;
      } else {
        field_start = c == delimiter_;
        ++off;
      }
    }
  }
  return ret;
}

int CsvRecordReader::split_fields(const int64_t rec_len, ObIArray<CsvField> &fields)
{
  int ret = OB_SUCCESS;
  char *p = buf_ + pos_;
  int64_t i = 0;
  bool done = false;
  fields.reuse();
  while (OB_SUCC(ret) && !done) {
    if (i < rec_len && 0 != quote_ && p[i] == quote_) {
      const int64_t start = i;
      int64_t w = i;
      bool closed = false;
      ++i;
      while (i < rec_len && !closed) {
        const char c = p[i];
        if (0 != escape_ && escape_ != quote_ && c == escape_ && i + 1 < rec_len) {
          p[w++] = p[i + 1];
          i += 2;
        } else if (c == quote_) {
          if (escape_ == quote_ && i + 1 < rec_len && p[i + 1] == quote_) {
            p[w++] = quote_;
            i += 2;
          } else {
            closed = true;
            ++i;
          }
        } else {
          p[w++] = c;
          ++i;
        }
      }
      if (!closed) {
        ret = OB_ERR_WRONG_VALUE_COUNT_ON_ROW;
        FileSqlUtil::set_user_error(ret, "read_csv: unterminated quoted field at line %ld", line_no_);
      } else if (i < rec_len && p[i] != delimiter_) {
        ret = OB_ERR_WRONG_VALUE_COUNT_ON_ROW;
        FileSqlUtil::set_user_error(ret, "read_csv: unexpected character after closing quote at line %ld",
                                    line_no_);
      } else if (OB_FAIL(fields.push_back(CsvField(p + start, w - start, true)))) {
      }
    } else {
      const int64_t start = i;
      while (i < rec_len && p[i] != delimiter_) {
        ++i;
      }
      if (OB_FAIL(fields.push_back(CsvField(p + start, i - start, false)))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (i >= rec_len) {
      done = true;
    } else {
      ++i; // skip delimiter, a trailing delimiter yields an empty last field
      if (fields.count() > FileSqlLimits::MAX_COLUMN_COUNT) {
        ret = OB_SIZE_OVERFLOW;
        FileSqlUtil::set_user_error(ret, "read_csv: line %ld has more than %ld fields",
                                    line_no_, FileSqlLimits::MAX_COLUMN_COUNT);
      }
    }
  }
  return ret;
}

int CsvRecordReader::next_record(ObIArray<CsvField> &fields, int64_t &line_no)
{
  int ret = OB_SUCCESS;
  bool found = false;
  bool has = false;
  if (OB_ISNULL(file_) || OB_ISNULL(buf_)) {
    ret = OB_NOT_INIT;
  }
  while (OB_SUCC(ret) && !found) {
    if (!bom_checked_) {
      if (OB_FAIL(ensure_data(2, has))) {
      } else {
        if (end_ - pos_ >= 3 && '\xEF' == buf_[pos_] && '\xBB' == buf_[pos_ + 1]
            && '\xBF' == buf_[pos_ + 2]) {
          pos_ += 3;
        }
        bom_checked_ = true;
      }
    }
    int64_t rec_len = 0;
    int64_t next_off = 0;
    int64_t newlines = 0;
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ensure_data(0, has))) {
    } else if (!has) {
      ret = OB_ITER_END;
    } else if (OB_FAIL(find_record_end(rec_len, next_off, newlines))) {
    } else {
      line_no = line_no_;
      if (rec_len > 0 && OB_FAIL(split_fields(rec_len, fields))) {
      } else {
        line_no_ += newlines;
        pos_ += next_off;
        found = rec_len > 0;
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// LineReader
// ---------------------------------------------------------------------------------------

LineReader::LineReader()
  : file_(NULL), max_line_size_(FileSqlLimits::DEFAULT_MAX_OBJECT_SIZE), buf_(NULL), buf_cap_(0),
    pos_(0), end_(0), eof_(false), bom_checked_(false), line_no_(0)
{}

void LineReader::destroy()
{
  if (NULL != buf_) {
    ob_free(buf_);
    buf_ = NULL;
  }
  buf_cap_ = 0;
  file_ = NULL;
}

int LineReader::init(LocalFileReader &file, const int64_t max_line_size)
{
  int ret = OB_SUCCESS;
  destroy();
  file_ = &file;
  max_line_size_ = max_line_size;
  buf_cap_ = 2 * FileSqlLimits::READ_BLOCK_SIZE;
  if (OB_ISNULL(buf_ = static_cast<char *>(ob_malloc(buf_cap_, ObMemAttr("FileSqlLine"))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    pos_ = 0;
    end_ = 0;
    eof_ = false;
    bom_checked_ = false;
    line_no_ = 0;
  }
  return ret;
}

int LineReader::rewind()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(file_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(file_->rewind())) {
  } else {
    pos_ = 0;
    end_ = 0;
    eof_ = false;
    bom_checked_ = false;
    line_no_ = 0;
  }
  return ret;
}

int LineReader::next_line(const char *&ptr, int64_t &len, int64_t &line_no)
{
  int ret = OB_SUCCESS;
  bool found = false;
  int64_t scanned = 0;
  if (OB_ISNULL(file_) || OB_ISNULL(buf_)) {
    ret = OB_NOT_INIT;
  }
  while (OB_SUCC(ret) && !found) {
    const char *nl = NULL;
    if (pos_ + scanned < end_) {
      nl = static_cast<const char *>(memchr(buf_ + pos_ + scanned, '\n', end_ - pos_ - scanned));
    }
    if (NULL != nl) {
      len = nl - (buf_ + pos_);
      ptr = buf_ + pos_;
      pos_ += len + 1;
      found = true;
    } else if (eof_) {
      if (pos_ >= end_) {
        ret = OB_ITER_END;
      } else {
        len = end_ - pos_;
        ptr = buf_ + pos_;
        pos_ = end_;
        found = true;
      }
    } else {
      scanned = end_ - pos_;
      if (scanned > max_line_size_) {
        ret = OB_ERR_TOO_BIG_ROWSIZE;
        FileSqlUtil::set_user_error(ret, "read_json: line %ld exceeds maximum_object_size (%ld bytes)",
                                    line_no_ + 1, max_line_size_);
      } else if (end_ == buf_cap_) {
        if (pos_ > 0) {
          MEMMOVE(buf_, buf_ + pos_, end_ - pos_);
          end_ -= pos_;
          pos_ = 0;
        } else {
          const int64_t new_cap = buf_cap_ * 2;
          char *new_buf = NULL;
          if (OB_ISNULL(new_buf = static_cast<char *>(ob_malloc(new_cap, ObMemAttr("FileSqlLine"))))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(new_buf, buf_, end_);
            ob_free(buf_);
            buf_ = new_buf;
            buf_cap_ = new_cap;
          }
        }
      }
      if (OB_SUCC(ret)) {
        int64_t read_len = 0;
        if (OB_FAIL(file_->read(buf_ + end_, buf_cap_ - end_, read_len))) {
        } else if (0 == read_len) {
          eof_ = true;
        } else {
          end_ += read_len;
        }
      }
    }
    if (OB_SUCC(ret) && found) {
      ++line_no_;
      line_no = line_no_;
      if (len > max_line_size_) {
        ret = OB_ERR_TOO_BIG_ROWSIZE;
        FileSqlUtil::set_user_error(ret, "read_json: line %ld exceeds maximum_object_size (%ld bytes)",
                                    line_no_, max_line_size_);
      } else {
        if (!bom_checked_) {
          bom_checked_ = true;
          if (len >= 3 && '\xEF' == ptr[0] && '\xBB' == ptr[1] && '\xBF' == ptr[2]) {
            ptr += 3;
            len -= 3;
          }
        }
        if (len > 0 && '\r' == ptr[len - 1]) {
          --len;
        }
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// JsonLineParser
// ---------------------------------------------------------------------------------------

static inline void skip_json_ws(const char *ptr, const int64_t len, int64_t &pos)
{
  while (pos < len && (' ' == ptr[pos] || '\t' == ptr[pos] || '\r' == ptr[pos] || '\n' == ptr[pos])) {
    ++pos;
  }
}

static int json_syntax_error(const int64_t line_no, const int64_t pos, const char *what)
{
  return FileSqlUtil::set_user_error(OB_ERR_INVALID_JSON_TEXT,
                                     "read_json: invalid JSON at line %ld, position %ld: %s",
                                     line_no, pos + 1, what);
}

static inline int hex_value(const char c)
{
  int v = -1;
  if (c >= '0' && c <= '9') {
    v = c - '0';
  } else if (c >= 'a' && c <= 'f') {
    v = c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    v = c - 'A' + 10;
  }
  return v;
}

int JsonLineParser::parse_string(const char *ptr, const int64_t len, int64_t &pos,
                                 const int64_t line_no, ObString &str)
{
  int ret = OB_SUCCESS;
  // ptr[pos] == '"'
  const int64_t start = pos + 1;
  int64_t i = start;
  bool has_escape = false;
  bool closed = false;
  while (OB_SUCC(ret) && i < len && !closed) {
    const unsigned char c = static_cast<unsigned char>(ptr[i]);
    if ('"' == c) {
      closed = true;
    } else if ('\\' == c) {
      has_escape = true;
      i += 2;
    } else if (c < 0x20) {
      ret = json_syntax_error(line_no, i, "control character in string");
    } else {
      ++i;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!closed || i > len) {
    ret = json_syntax_error(line_no, pos, "unterminated string");
  } else if (!has_escape) {
    str.assign_ptr(ptr + start, static_cast<int32_t>(i - start));
    pos = i + 1;
  } else {
    char *buf = static_cast<char *>(scratch_.alloc(i - start + 1));
    int64_t w = 0;
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    for (int64_t j = start; OB_SUCC(ret) && j < i; ++j) {
      if ('\\' != ptr[j]) {
        buf[w++] = ptr[j];
      } else {
        ++j;
        switch (ptr[j]) {
          case '"': buf[w++] = '"'; break;
          case '\\': buf[w++] = '\\'; break;
          case '/': buf[w++] = '/'; break;
          case 'b': buf[w++] = '\b'; break;
          case 'f': buf[w++] = '\f'; break;
          case 'n': buf[w++] = '\n'; break;
          case 'r': buf[w++] = '\r'; break;
          case 't': buf[w++] = '\t'; break;
          case 'u': {
            uint32_t cp = 0;
            bool ok = j + 4 < i;
            for (int64_t k = 1; ok && k <= 4; ++k) {
              const int h = hex_value(ptr[j + k]);
              ok = h >= 0;
              cp = (cp << 4) | static_cast<uint32_t>(h);
            }
            if (ok) {
              j += 4;
              if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo = 0;
                ok = j + 6 < i && '\\' == ptr[j + 1] && 'u' == ptr[j + 2];
                for (int64_t k = 3; ok && k <= 6; ++k) {
                  const int h = hex_value(ptr[j + k]);
                  ok = h >= 0;
                  lo = (lo << 4) | static_cast<uint32_t>(h);
                }
                ok = ok && lo >= 0xDC00 && lo <= 0xDFFF;
                if (ok) {
                  j += 6;
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
              } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                ok = false;
              }
            }
            if (!ok) {
              ret = json_syntax_error(line_no, j, "invalid \\u escape");
            } else if (cp < 0x80) {
              buf[w++] = static_cast<char>(cp);
            } else if (cp < 0x800) {
              buf[w++] = static_cast<char>(0xC0 | (cp >> 6));
              buf[w++] = static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
              buf[w++] = static_cast<char>(0xE0 | (cp >> 12));
              buf[w++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
              buf[w++] = static_cast<char>(0x80 | (cp & 0x3F));
            } else {
              buf[w++] = static_cast<char>(0xF0 | (cp >> 18));
              buf[w++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
              buf[w++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
              buf[w++] = static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
          }
          default:
            ret = json_syntax_error(line_no, j, "invalid escape sequence");
            break;
        }
      }
    }
    if (OB_SUCC(ret)) {
      str.assign_ptr(buf, static_cast<int32_t>(w));
      pos = i + 1;
    }
  }
  return ret;
}

int JsonLineParser::parse_number(const char *ptr, const int64_t len, int64_t &pos,
                                 const int64_t line_no, JsonField &field)
{
  int ret = OB_SUCCESS;
  const int64_t start = pos;
  int64_t i = pos;
  bool is_int = true;
  if (i < len && '-' == ptr[i]) {
    ++i;
  }
  if (i < len && '0' == ptr[i]) {
    ++i;
  } else if (i < len && ptr[i] >= '1' && ptr[i] <= '9') {
    while (i < len && is_digit(ptr[i])) {
      ++i;
    }
  } else {
    ret = json_syntax_error(line_no, i, "invalid number");
  }
  if (OB_SUCC(ret) && i < len && '.' == ptr[i]) {
    is_int = false;
    ++i;
    if (i >= len || !is_digit(ptr[i])) {
      ret = json_syntax_error(line_no, i, "invalid number");
    }
    while (i < len && is_digit(ptr[i])) {
      ++i;
    }
  }
  if (OB_SUCC(ret) && i < len && ('e' == ptr[i] || 'E' == ptr[i])) {
    is_int = false;
    ++i;
    if (i < len && ('+' == ptr[i] || '-' == ptr[i])) {
      ++i;
    }
    if (i >= len || !is_digit(ptr[i])) {
      ret = json_syntax_error(line_no, i, "invalid number");
    }
    while (i < len && is_digit(ptr[i])) {
      ++i;
    }
  }
  if (OB_SUCC(ret)) {
    int64_t iv = 0;
    double dv = 0;
    field.text_.assign_ptr(ptr + start, static_cast<int32_t>(i - start));
    if (is_int && FileSqlUtil::parse_bigint(field.text_.ptr(), field.text_.length(), iv)) {
      field.kind_ = JsonValueKind::INTEGER;
    } else if (FileSqlUtil::parse_double(field.text_.ptr(), field.text_.length(), dv)) {
      field.kind_ = JsonValueKind::NUMBER;
    } else {
      ret = json_syntax_error(line_no, start, "number out of range");
    }
    pos = i;
  }
  return ret;
}

int JsonLineParser::parse_value(const char *ptr, const int64_t len, int64_t &pos,
                                const int64_t line_no, JsonField &field)
{
  int ret = OB_SUCCESS;
  if (pos >= len) {
    ret = json_syntax_error(line_no, pos, "missing value");
  } else {
    const char c = ptr[pos];
    if ('"' == c) {
      field.kind_ = JsonValueKind::STRING;
      ret = parse_string(ptr, len, pos, line_no, field.text_);
    } else if ('-' == c || is_digit(c)) {
      ret = parse_number(ptr, len, pos, line_no, field);
    } else if (len - pos >= 4 && 0 == MEMCMP(ptr + pos, "null", 4)) {
      field.kind_ = JsonValueKind::NULL_VALUE;
      pos += 4;
    } else if (len - pos >= 4 && 0 == MEMCMP(ptr + pos, "true", 4)) {
      field.kind_ = JsonValueKind::BOOL_TRUE;
      field.text_.assign_ptr("true", 4);
      pos += 4;
    } else if (len - pos >= 5 && 0 == MEMCMP(ptr + pos, "false", 5)) {
      field.kind_ = JsonValueKind::BOOL_FALSE;
      field.text_.assign_ptr("false", 5);
      pos += 5;
    } else if ('{' == c || '[' == c) {
      ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
          "read_json: nested %s in field '%.*s' at line %ld is not supported, only flat objects can be read",
          '{' == c ? "object" : "array", field.key_.length(), field.key_.ptr(), line_no);
    } else {
      ret = json_syntax_error(line_no, pos, "invalid value");
    }
  }
  return ret;
}

int JsonLineParser::parse(const char *ptr, const int64_t len, const int64_t line_no,
                          ObIArray<JsonField> &fields)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  fields.reuse();
  scratch_.reuse();
  if (!FileSqlUtil::is_valid_utf8(ptr, len)) {
    ret = FileSqlUtil::set_user_error(OB_ERR_INVALID_CHARACTER_STRING,
                                      "read_json: invalid utf8mb4 character string at line %ld", line_no);
  } else {
    skip_json_ws(ptr, len, pos);
    if (pos >= len) {
      ret = json_syntax_error(line_no, pos, "empty line");
    } else if ('[' == ptr[pos]) {
      ret = FileSqlUtil::set_user_error(OB_NOT_SUPPORTED,
          "read_json: line %ld is a JSON array, only newline-delimited flat objects are supported", line_no);
    } else if ('{' != ptr[pos]) {
      ret = json_syntax_error(line_no, pos, "expected a JSON object");
    } else {
      ++pos;
      skip_json_ws(ptr, len, pos);
      bool done = false;
      if (pos < len && '}' == ptr[pos]) {
        ++pos;
        done = true;
      }
      while (OB_SUCC(ret) && !done) {
        JsonField field;
        skip_json_ws(ptr, len, pos);
        if (pos >= len || '"' != ptr[pos]) {
          ret = json_syntax_error(line_no, pos, "expected a string key");
        } else if (OB_FAIL(parse_string(ptr, len, pos, line_no, field.key_))) {
        } else {
          skip_json_ws(ptr, len, pos);
          if (pos >= len || ':' != ptr[pos]) {
            ret = json_syntax_error(line_no, pos, "expected ':'");
          } else {
            ++pos;
            skip_json_ws(ptr, len, pos);
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(parse_value(ptr, len, pos, line_no, field))) {
        } else if (OB_FAIL(fields.push_back(field))) {
        } else if (fields.count() > FileSqlLimits::MAX_COLUMN_COUNT) {
          ret = FileSqlUtil::set_user_error(OB_SIZE_OVERFLOW, "read_json: line %ld has more than %ld fields",
                                            line_no, FileSqlLimits::MAX_COLUMN_COUNT);
        } else {
          skip_json_ws(ptr, len, pos);
          if (pos < len && ',' == ptr[pos]) {
            ++pos;
          } else if (pos < len && '}' == ptr[pos]) {
            ++pos;
            done = true;
          } else {
            ret = json_syntax_error(line_no, pos, "expected ',' or '}'");
          }
        }
      }
      if (OB_SUCC(ret)) {
        skip_json_ws(ptr, len, pos);
        if (pos < len) {
          ret = json_syntax_error(line_no, pos, "unexpected characters after the object");
        }
      }
    }
  }
  return ret;
}

int JsonLineParser::json_value_type(const JsonField &field, FileColumnType &type, int16_t &scale)
{
  int ret = OB_SUCCESS;
  scale = 0;
  switch (field.kind_) {
    case JsonValueKind::NULL_VALUE: type = FileColumnType::NULL_TYPE; break;
    case JsonValueKind::BOOL_TRUE:
    case JsonValueKind::BOOL_FALSE: type = FileColumnType::BOOLEAN; break;
    case JsonValueKind::INTEGER: type = FileColumnType::BIGINT; break;
    case JsonValueKind::NUMBER: type = FileColumnType::DOUBLE; break;
    case JsonValueKind::STRING:
      type = FileSqlUtil::classify_json_string(field.text_.ptr(), field.text_.length(), scale);
      break;
    default: ret = OB_ERR_UNEXPECTED; break;
  }
  return ret;
}

bool JsonLineParser::convert_json(const FileColumnType type, const JsonField &field, FileValue &value)
{
  bool ok = true;
  value.reset();
  if (JsonValueKind::NULL_VALUE == field.kind_ || JsonValueKind::NONE == field.kind_
      || FileColumnType::NULL_TYPE == type) {
    value.is_null_ = true;
  } else {
    value.is_null_ = false;
    switch (type) {
      case FileColumnType::VARCHAR:
        value.str_ = field.text_;
        break;
      case FileColumnType::BOOLEAN:
        ok = JsonValueKind::BOOL_TRUE == field.kind_ || JsonValueKind::BOOL_FALSE == field.kind_;
        value.int_ = JsonValueKind::BOOL_TRUE == field.kind_ ? 1 : 0;
        break;
      case FileColumnType::BIGINT:
        ok = JsonValueKind::INTEGER == field.kind_
             && FileSqlUtil::parse_bigint(field.text_.ptr(), field.text_.length(), value.int_);
        break;
      case FileColumnType::DOUBLE:
        ok = (JsonValueKind::INTEGER == field.kind_ || JsonValueKind::NUMBER == field.kind_)
             && FileSqlUtil::parse_double(field.text_.ptr(), field.text_.length(), value.double_);
        break;
      case FileColumnType::DATE:
      case FileColumnType::DATETIME:
        ok = JsonValueKind::STRING == field.kind_
             && FileSqlUtil::convert_text(type, field.text_.ptr(), field.text_.length(), value);
        break;
      default:
        ok = false;
        break;
    }
  }
  return ok;
}

// ---------------------------------------------------------------------------------------
// schema inference
// ---------------------------------------------------------------------------------------

void FileSchemaInferrer::fill_column(const FileColumnInferState &state, const bool all_varchar,
                                     FileColumnDesc &column)
{
  column.set_type(all_varchar ? FileColumnType::VARCHAR : state.type_);
  column.nullable_ = state.has_null_ || !state.has_value_;
  column.max_length_ = state.max_chars_ > 0 ? state.max_chars_ : 1;
  column.scale_ = FileColumnType::DATETIME == column.get_type() ? state.max_scale_ : 0;
}

static inline bool is_csv_null(const FileTableDesc &desc, const CsvField &field)
{
  return !field.quoted_
         && (0 == field.len_
             || (field.len_ == desc.null_str_.length()
                 && 0 == MEMCMP(field.ptr_, desc.null_str_.ptr(), field.len_)));
}

static int check_csv_field(const FileTableDesc &desc, const CsvField &field, const int64_t line_no,
                           const int64_t col_idx, int64_t &chars)
{
  int ret = OB_SUCCESS;
  if (!FileSqlUtil::is_valid_utf8(field.ptr_, field.len_)) {
    ret = FileSqlUtil::set_user_error(OB_ERR_INVALID_CHARACTER_STRING,
        "read_csv: invalid utf8mb4 character string at line %ld, column %ld of '%.*s'",
        line_no, col_idx + 1, desc.path_.length(), desc.path_.ptr());
  } else if ((chars = FileSqlUtil::utf8_char_count(field.ptr_, field.len_)) > FileSqlLimits::MAX_VARCHAR_CHARS) {
    ret = FileSqlUtil::set_user_error(OB_ERR_DATA_TOO_LONG,
        "read_csv: field at line %ld, column %ld is longer than %ld characters",
        line_no, col_idx + 1, FileSqlLimits::MAX_VARCHAR_CHARS);
  }
  return ret;
}

// ignore_errors skips records holding invalid utf8mb4, whichever columns are projected
static bool has_invalid_utf8(const ObIArray<CsvField> &fields)
{
  bool invalid = false;
  for (int64_t i = 0; !invalid && i < fields.count(); ++i) {
    invalid = !FileSqlUtil::is_valid_utf8(fields.at(i).ptr_, fields.at(i).len_);
  }
  return invalid;
}

// Without a delimiter option (delimiter_ 0) a file keeps ',' when its first record holds a
// comma. Otherwise '|', ';' and tab are tried in order: the first one that splits the first
// records into the same number (> 1) of fields is used.
static int sniff_csv_delimiter(LocalFileReader &file, FileTableDesc &desc)
{
  int ret = OB_SUCCESS;
  static const char CANDIDATES[] = {',', '|', ';', '\t'};
  const int64_t SAMPLE_RECORDS = 32;
  char chosen = ',';
  bool done = false;
  for (int64_t c = 0; OB_SUCC(ret) && !done && c < static_cast<int64_t>(sizeof(CANDIDATES)); ++c) {
    CsvRecordReader reader;
    ObSEArray<CsvField, 64> fields;
    int64_t line_no = 0;
    int64_t first_cols = -1;
    bool consistent = true;
    desc.delimiter_ = CANDIDATES[c];
    if (CANDIDATES[c] == desc.quote_ || CANDIDATES[c] == desc.escape_) {
      consistent = false;
    } else if (OB_FAIL(file.rewind())) {
    } else if (OB_FAIL(reader.init(file, desc))) {
    } else {
      for (int64_t i = 0; consistent && i < desc.skip_rows_ + SAMPLE_RECORDS; ++i) {
        const int tmp_ret = reader.next_record(fields, line_no);
        if (OB_ITER_END == tmp_ret) {
          break;
        } else if (OB_SUCCESS != tmp_ret) {
          // the scan with the chosen delimiter reports the error
          consistent = false;
        } else if (i < desc.skip_rows_) {
        } else if (first_cols < 0) {
          first_cols = fields.count();
        } else if (fields.count() != first_cols) {
          consistent = false;
        }
      }
      reader.destroy();
    }
    if (OB_FAIL(ret)) {
    } else if (0 == c) {
      // a comma that is the quote or escape character is never kept
      done = first_cols >= 0 && 1 != first_cols;
    } else if (consistent && first_cols > 1) {
      chosen = CANDIDATES[c];
      done = true;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (chosen == desc.quote_ || chosen == desc.escape_) {
    ret = FileSqlUtil::set_user_error(OB_INVALID_ARGUMENT,
        "read_csv: %s and delimiter must be different", chosen == desc.quote_ ? "quote" : "escape");
  } else {
    desc.delimiter_ = chosen;
    ret = file.rewind();
  }
  return ret;
}

int FileSchemaInferrer::infer_csv(ObIAllocator &alloc, FileTableDesc &desc)
{
  int ret = OB_SUCCESS;
  LocalFileReader file;
  CsvRecordReader reader;
  ObSEArray<CsvField, 64> fields;
  ObSEArray<ObString, 64> first_values;
  ObSEArray<bool, 64> first_nulls;
  ObSEArray<FileColumnInferState, 64> first_states;
  ObSEArray<FileColumnInferState, 64> rest_states;
  int64_t line_no = 0;
  int64_t data_rows = 0;
  int64_t rejected = 0;
  int64_t ncols = 0;
  bool empty_file = false;
  if (OB_FAIL(file.open(desc.real_path_, &desc.identity_))) {
  } else if (0 == desc.delimiter_ && OB_FAIL(sniff_csv_delimiter(file, desc))) {
  } else if (OB_FAIL(reader.init(file, desc))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < desc.skip_rows_; ++i) {
    if (OB_FAIL(reader.next_record(fields, line_no))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        break;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(reader.next_record(fields, line_no))) {
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
      empty_file = true;
    }
  } else {
    ncols = fields.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < ncols; ++i) {
      const CsvField &f = fields.at(i);
      FileColumnInferState state;
      ObString value;
      int64_t chars = 0;
      int16_t scale = 0;
      if (OB_FAIL(check_csv_field(desc, f, line_no, i, chars))) {
      } else if (OB_FAIL(ob_write_string(alloc, ObString(f.len_, f.ptr_), value))) {
      } else {
        const bool is_null = is_csv_null(desc, f);
        if (is_null) {
          state.add_null();
        } else {
          state.add_value(FileSqlUtil::classify_text(f.ptr_, f.len_, scale), chars, scale);
        }
        if (OB_FAIL(first_values.push_back(value))) {
        } else if (OB_FAIL(first_nulls.push_back(is_null))) {
        } else if (OB_FAIL(first_states.push_back(state))) {
        } else if (OB_FAIL(rest_states.push_back(FileColumnInferState()))) {
        }
      }
    }
  }
  while (OB_SUCC(ret) && !empty_file) {
    if (OB_FAIL(reader.next_record(fields, line_no))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        break;
      }
    } else if (fields.count() != ncols || (desc.ignore_errors_ && has_invalid_utf8(fields))) {
      if (desc.ignore_errors_) {
        ++rejected;
      } else {
        ret = FileSqlUtil::set_user_error(OB_ERR_WRONG_VALUE_COUNT_ON_ROW,
            "read_csv: bad row at line %ld of '%.*s': expected %ld fields but found %ld",
            line_no, desc.path_.length(), desc.path_.ptr(), ncols, fields.count());
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < ncols; ++i) {
        const CsvField &f = fields.at(i);
        int64_t chars = 0;
        int16_t scale = 0;
        if (OB_FAIL(check_csv_field(desc, f, line_no, i, chars))) {
        } else if (is_csv_null(desc, f)) {
          rest_states.at(i).add_null();
        } else {
          rest_states.at(i).add_value(FileSqlUtil::classify_text(f.ptr_, f.len_, scale), chars, scale);
        }
      }
      if (OB_SUCC(ret) && 0 == (++data_rows % FileSqlLimits::CHECK_STATUS_INTERVAL)) {
        ret = THIS_WORKER.check_status();
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(file.check_unchanged())) {
  }
  if (OB_FAIL(ret)) {
  } else if (empty_file) {
    FileColumnDesc col;
    col.set_type(FileColumnType::VARCHAR);
    col.nullable_ = true;
    col.max_length_ = 1;
    if (OB_FAIL(desc.columns_.init(1))) {
    } else if (OB_FAIL(desc.columns_.push_back(col))) {
    } else if (OB_FAIL(FileSqlUtil::make_unique_names(alloc, desc.columns_))) {
    } else {
      desc.has_header_ = false;
      desc.row_count_ = 0;
    }
  } else {
    bool header = false;
    if (desc.header_option_ >= 0) {
      header = 1 == desc.header_option_;
    } else if (data_rows > 0) {
      bool any_typed = false;
      bool all_first_present = true;
      for (int64_t i = 0; i < ncols; ++i) {
        const FileColumnType rest_type = rest_states.at(i).type_;
        all_first_present = all_first_present && !first_nulls.at(i);
        if (FileColumnType::VARCHAR != rest_type && FileColumnType::NULL_TYPE != rest_type) {
          any_typed = true;
          if (first_states.at(i).has_value_
              && widen_file_column_type(rest_type, first_states.at(i).type_) != rest_type) {
            header = true;
          }
        }
      }
      if (!any_typed) {
        header = all_first_present;
      }
    }
    if (OB_FAIL(desc.columns_.init(ncols))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < ncols; ++i) {
      FileColumnDesc col;
      FileColumnInferState state = rest_states.at(i);
      if (header) {
        col.source_name_ = first_nulls.at(i) ? ObString() : first_values.at(i);
      } else {
        state.merge(first_states.at(i));
      }
      fill_column(state, desc.all_varchar_, col);
      col.source_idx_ = static_cast<int32_t>(i);
      if (OB_FAIL(desc.columns_.push_back(col))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(FileSqlUtil::make_unique_names(alloc, desc.columns_))) {
    } else {
      desc.has_header_ = header;
      desc.row_count_ = data_rows + (header ? 0 : 1);
      desc.rejected_rows_ = rejected;
    }
  }
  return ret;
}

int FileSchemaInferrer::infer_json(ObIAllocator &alloc, FileTableDesc &desc)
{
  int ret = OB_SUCCESS;
  LocalFileReader file;
  LineReader reader;
  JsonLineParser parser;
  ObSEArray<JsonField, 32> fields;
  ObSEArray<ObString, 32> keys;
  ObSEArray<FileColumnInferState, 32> states;
  ObSEArray<int64_t, 32> present;
  ObSEArray<int64_t, 32> last_row;
  hash::ObHashMap<ObString, int64_t> key_map;
  int64_t rows = 0;
  int64_t rejected = 0;
  if (OB_FAIL(key_map.create(1024, "FileSqlJsonKey"))) {
  } else if (OB_FAIL(file.open(desc.real_path_, &desc.identity_))) {
  } else if (OB_FAIL(reader.init(file, desc.max_object_size_))) {
  }
  while (OB_SUCC(ret)) {
    const char *line = NULL;
    int64_t len = 0;
    int64_t line_no = 0;
    int64_t pos = 0;
    if (OB_FAIL(reader.next_line(line, len, line_no))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        break;
      }
    } else if (FALSE_IT(skip_json_ws(line, len, pos))) {
    } else if (pos >= len) {
      // blank line
    } else if (OB_FAIL(parser.parse(line, len, line_no, fields))) {
      if (desc.ignore_errors_ && (OB_ERR_INVALID_JSON_TEXT == ret || OB_ERR_INVALID_CHARACTER_STRING == ret)) {
        ret = OB_SUCCESS;
        ++rejected;
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < fields.count(); ++i) {
        const JsonField &f = fields.at(i);
        int64_t idx = -1;
        FileColumnType type = FileColumnType::NULL_TYPE;
        int16_t scale = 0;
        int tmp_ret = key_map.get_refactored(f.key_, idx);
        if (OB_HASH_NOT_EXIST == tmp_ret) {
          ObString key;
          idx = keys.count();
          if (idx >= FileSqlLimits::MAX_COLUMN_COUNT) {
            ret = FileSqlUtil::set_user_error(OB_SIZE_OVERFLOW, "read_json: more than %ld distinct fields",
                                              FileSqlLimits::MAX_COLUMN_COUNT);
          } else if (OB_FAIL(ob_write_string(alloc, f.key_, key))) {
          } else if (OB_FAIL(keys.push_back(key))) {
          } else if (OB_FAIL(states.push_back(FileColumnInferState()))) {
          } else if (OB_FAIL(present.push_back(0))) {
          } else if (OB_FAIL(last_row.push_back(-1))) {
          } else if (OB_FAIL(key_map.set_refactored(key, idx))) {
          }
        } else if (OB_SUCCESS != tmp_ret) {
          ret = tmp_ret;
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(JsonLineParser::json_value_type(f, type, scale))) {
        } else {
          FileColumnInferState &state = states.at(idx);
          if (FileColumnType::NULL_TYPE == type) {
            state.add_null();
          } else {
            const int64_t chars = FileSqlUtil::utf8_char_count(f.text_.ptr(), f.text_.length());
            if (chars > FileSqlLimits::MAX_VARCHAR_CHARS) {
              ret = FileSqlUtil::set_user_error(OB_ERR_DATA_TOO_LONG,
                  "read_json: value of field '%.*s' at line %ld is longer than %ld characters",
                  f.key_.length(), f.key_.ptr(), line_no, FileSqlLimits::MAX_VARCHAR_CHARS);
            } else {
              state.add_value(type, chars, scale);
            }
          }
          if (last_row.at(idx) != rows) {
            last_row.at(idx) = rows;
            ++present.at(idx);
          }
        }
      }
      if (OB_SUCC(ret) && 0 == (++rows % FileSqlLimits::CHECK_STATUS_INTERVAL)) {
        ret = THIS_WORKER.check_status();
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(file.check_unchanged())) {
  }
  if (OB_SUCC(ret)) {
    const int64_t ncols = keys.count() > 0 ? keys.count() : 1;
    if (OB_FAIL(desc.columns_.init(ncols))) {
    } else if (keys.empty()) {
      FileColumnDesc col;
      col.set_type(FileColumnType::VARCHAR);
      ret = desc.columns_.push_back(col);
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); ++i) {
      FileColumnDesc col;
      FileColumnInferState state = states.at(i);
      if (present.at(i) < rows) {
        state.add_null();
      }
      fill_column(state, desc.all_varchar_, col);
      col.source_name_ = keys.at(i);
      col.source_idx_ = static_cast<int32_t>(i);
      if (OB_FAIL(desc.columns_.push_back(col))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(FileSqlUtil::make_unique_names(alloc, desc.columns_))) {
    } else {
      desc.row_count_ = rows;
      desc.rejected_rows_ = rejected;
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// glob
// ---------------------------------------------------------------------------------------

bool FileGlob::has_wildcard(const ObString &str)
{
  bool found = false;
  for (int64_t i = 0; !found && i < str.length(); ++i) {
    found = '*' == str[i] || '?' == str[i] || '[' == str[i];
  }
  return found;
}

namespace
{
struct GlobContext
{
  GlobContext(ObIAllocator &alloc, ObIArray<ObString> &files, const ObString &priv)
    : alloc_(alloc), files_(files), priv_(priv), components_(), user_base_(), visited_(0) {}
  ObIAllocator &alloc_;
  ObIArray<ObString> &files_;
  ObString priv_;
  ObSEArray<ObString, 16> components_;
  ObString user_base_;
  int64_t visited_;
};

inline const char *cstr(const ObSqlString &s)
{
  return s.empty() ? "" : s.ptr();
}

bool is_inside_dir(const ObString &path, const ObString &dir)
{
  return dir == "/"
         || (path.prefix_match(dir) && (path.length() == dir.length() || path[dir.length()] == '/'));
}

int add_glob_match(GlobContext &ctx, const ObSqlString &real_dir_path, const ObSqlString &user_path)
{
  int ret = OB_SUCCESS;
  char real_buf[PATH_MAX + 1];
  struct stat st;
  if (NULL == ::realpath(real_dir_path.ptr(), real_buf)) {
    // dangling link, skip
  } else if (0 != ::stat(real_buf, &st) || !S_ISREG(st.st_mode)) {
    // only regular files
  } else if (!FileSqlUtil::is_path_allowed(ctx.priv_, ObString(real_buf))) {
    // a link that escapes secure_file_priv is silently skipped
  } else if (ctx.files_.count() >= FileSqlLimits::MAX_GLOB_FILES) {
    ret = FileSqlUtil::set_user_error(OB_SIZE_OVERFLOW, "glob: more than %ld files match the pattern",
                                      FileSqlLimits::MAX_GLOB_FILES);
  } else {
    ObString path;
    if (OB_FAIL(ob_write_string(ctx.alloc_, user_path.string(), path))) {
    } else if (OB_FAIL(ctx.files_.push_back(path))) {
    }
  }
  return ret;
}

int glob_walk(GlobContext &ctx, const ObSqlString &dir, const ObSqlString &user_dir,
              const int64_t comp_idx, const int64_t depth)
{
  int ret = OB_SUCCESS;
  const int64_t ncomps = ctx.components_.count();
  if (depth > FileSqlLimits::MAX_GLOB_DEPTH) {
    // stop descending
  } else if (comp_idx >= ncomps) {
    // pattern consumed
  } else if (0 == (++ctx.visited_ % 256) && OB_FAIL(THIS_WORKER.check_status())) {
  } else {
    const ObString &comp = ctx.components_.at(comp_idx);
    const bool is_last = comp_idx == ncomps - 1;
    const bool is_recursive = comp == "**";
    char pattern[NAME_MAX + 1];
    int64_t plen = comp.length() < NAME_MAX ? comp.length() : NAME_MAX;
    MEMCPY(pattern, comp.ptr(), plen);
    pattern[plen] = '\0';
    if (is_recursive && OB_FAIL(SMART_CALL(glob_walk(ctx, dir, user_dir, comp_idx + 1, depth + 1)))) {
    } else if (!is_recursive && !FileGlob::has_wildcard(comp)) {
      ObSqlString child;
      ObSqlString user_child;
      struct stat st;
      if (OB_FAIL(child.append_fmt("%s/%s", cstr(dir), pattern))) {
      } else if (OB_FAIL(user_child.append_fmt("%s/%s", cstr(user_dir), pattern))) {
      } else if (0 != ::stat(child.ptr(), &st)) {
      } else if (is_last) {
        ret = add_glob_match(ctx, child, user_child);
      } else if (S_ISDIR(st.st_mode)) {
        ret = SMART_CALL(glob_walk(ctx, child, user_child, comp_idx + 1, depth + 1));
      }
    } else {
      DIR *d = ::opendir(dir.empty() ? "/" : dir.ptr());
      ObSEArray<ObString, 64> names;
      ObArenaAllocator name_alloc("FileSqlGlob");
      if (NULL != d) {
        struct dirent *ent = NULL;
        while (OB_SUCC(ret) && NULL != (ent = ::readdir(d))) {
          ObString name;
          if (0 == strcmp(ent->d_name, ".") || 0 == strcmp(ent->d_name, "..")) {
          } else if (is_recursive ? '.' == ent->d_name[0]
                                  : 0 != ::fnmatch(pattern, ent->d_name, FNM_PERIOD)) {
          } else if (OB_FAIL(ob_write_string(name_alloc, ObString(ent->d_name), name, true))) {
          } else if (OB_FAIL(names.push_back(name))) {
          }
        }
        ::closedir(d);
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
        ObSqlString child;
        ObSqlString user_child;
        struct stat st;
        if (OB_FAIL(child.append_fmt("%s/%s", cstr(dir), names.at(i).ptr()))) {
        } else if (OB_FAIL(user_child.append_fmt("%s/%s", cstr(user_dir), names.at(i).ptr()))) {
        } else if (is_recursive) {
          // '**' descends into real directories only, links are not followed
          if (0 == ::lstat(child.ptr(), &st) && S_ISDIR(st.st_mode)) {
            ret = SMART_CALL(glob_walk(ctx, child, user_child, comp_idx, depth + 1));
          }
        } else if (0 != ::stat(child.ptr(), &st)) {
        } else if (is_last) {
          ret = add_glob_match(ctx, child, user_child);
        } else if (S_ISDIR(st.st_mode)) {
          ret = SMART_CALL(glob_walk(ctx, child, user_child, comp_idx + 1, depth + 1));
        }
      }
    }
  }
  return ret;
}
} // anonymous namespace

int FileGlob::glob(const ObString &pattern, const ObString &secure_file_priv, ObIAllocator &alloc,
                   ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  GlobContext ctx(alloc, files, secure_file_priv);
  ObSqlString abs_pattern;
  ObSqlString base;
  ObSqlString user_base;
  char cwd[PATH_MAX + 1];
  char real_base[PATH_MAX + 1];
  char priv_buf[PATH_MAX + 1];
  char real_priv[PATH_MAX + 1];
  files.reuse();
  if (pattern.empty() || pattern.length() >= PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "glob: pattern");
  } else if ('/' != pattern[0]) {
    if (NULL == ::getcwd(cwd, sizeof(cwd))) {
      ret = OB_IO_ERROR;
    } else if (OB_FAIL(abs_pattern.append_fmt("%s/%.*s", cwd, pattern.length(), pattern.ptr()))) {
    }
  } else if (OB_FAIL(abs_pattern.append(pattern))) {
  }
  // split into components, the leading ones without wildcards form the base directory
  int64_t first_wild = -1;
  if (OB_SUCC(ret)) {
    const char *p = abs_pattern.ptr();
    const int64_t len = abs_pattern.length();
    int64_t i = 0;
    while (OB_SUCC(ret) && i < len) {
      while (i < len && '/' == p[i]) {
        ++i;
      }
      const int64_t start = i;
      while (i < len && '/' != p[i]) {
        ++i;
      }
      if (i > start) {
        ObString comp(static_cast<int32_t>(i - start), p + start);
        if (comp == "..") {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "glob: '..' in pattern");
        } else if (comp == ".") {
        } else if (OB_FAIL(ctx.components_.push_back(comp))) {
        } else if (first_wild < 0 && (has_wildcard(comp) || comp == "**")) {
          first_wild = ctx.components_.count() - 1;
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    const int64_t base_comps = first_wild < 0 ? ctx.components_.count() - 1 : first_wild;
    if (OB_FAIL(base.append(""))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < base_comps; ++i) {
      ret = base.append_fmt("/%.*s", ctx.components_.at(i).length(), ctx.components_.at(i).ptr());
    }
    if (OB_SUCC(ret) && base.empty()) {
      ret = base.append("/");
    }
    if (OB_FAIL(ret)) {
    } else if (ctx.components_.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "glob: pattern");
    } else if (NULL == ::realpath(base.ptr(), real_base)) {
      // base directory does not exist: no files, unless it is outside secure_file_priv
      ObString real_path;
      bool exists = false;
      ObArenaAllocator tmp_alloc("FileSqlGlob");
      if (OB_SUCC(FileSqlUtil::canonicalize_path(base.string(), false, tmp_alloc, real_path, exists))
          && OB_FAIL(FileSqlUtil::check_secure_path(secure_file_priv, real_path))) {
      } else {
        ret = OB_SUCCESS;
      }
    } else {
      ObString real_base_str(real_base);
      bool allowed = FileSqlUtil::is_path_allowed(secure_file_priv, real_base_str);
      if (!allowed) {
        // the pattern may still reach into secure_file_priv from a parent directory
        if (!secure_file_priv.empty() && secure_file_priv.length() < PATH_MAX
            && 0 != secure_file_priv.case_compare("NULL")) {
          MEMCPY(priv_buf, secure_file_priv.ptr(), secure_file_priv.length());
          priv_buf[secure_file_priv.length()] = '\0';
          if (NULL != ::realpath(priv_buf, real_priv)
              && is_inside_dir(ObString(real_priv), real_base_str)) {
            allowed = true;
          }
        }
      }
      if (!allowed) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "FILE (path is outside secure_file_priv)");
      } else {
        ObSqlString walk_base;
        ObSqlString walk_user_base;
        const int64_t base_comps = first_wild < 0 ? ctx.components_.count() - 1 : first_wild;
        // walk below the base; the user visible prefix keeps the spelling of the pattern
        ObSEArray<ObString, 16> rest;
        for (int64_t i = base_comps; OB_SUCC(ret) && i < ctx.components_.count(); ++i) {
          ret = rest.push_back(ctx.components_.at(i));
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ctx.components_.assign(rest))) {
        } else if (OB_FAIL(walk_base.append(0 == strcmp(real_base, "/") ? "" : real_base))) {
        } else if (OB_FAIL(walk_user_base.append(base.string()))) {
        } else {
          if (walk_user_base.string() == "/") {
            walk_user_base.reset();
          }
          if (OB_FAIL(glob_walk(ctx, walk_base, walk_user_base, 0, 0))) {
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && files.count() > 1) {
    lib::ob_sort(&files.at(0), &files.at(0) + files.count());
  }
  return ret;
}

int FileSchemaInferrer::infer_glob(ObIAllocator &alloc, const ObString &secure_file_priv,
                                   FileTableDesc &desc)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 64> files;
  FileColumnDesc col;
  if (OB_FAIL(FileGlob::glob(desc.path_, secure_file_priv, alloc, files))) {
  } else {
    int64_t max_chars = 1;
    for (int64_t i = 0; i < files.count(); ++i) {
      const int64_t chars = FileSqlUtil::utf8_char_count(files.at(i).ptr(), files.at(i).length());
      max_chars = chars > max_chars ? chars : max_chars;
    }
    col.source_name_ = ObString("file");
    col.set_type(FileColumnType::VARCHAR);
    col.nullable_ = false;
    col.max_length_ = max_chars > PATH_MAX ? max_chars : PATH_MAX;
    if (OB_FAIL(desc.columns_.init(1))) {
    } else if (OB_FAIL(desc.columns_.push_back(col))) {
    } else if (OB_FAIL(FileSqlUtil::make_unique_names(alloc, desc.columns_))) {
    } else {
      desc.row_count_ = files.count();
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// COPY TO csv
// ---------------------------------------------------------------------------------------

static int append_copy_field(ObSqlString &buf, const ObString &value)
{
  return buf.append_fmt("%d:%.*s", value.length(), value.length(), value.ptr());
}

static int read_copy_field(const ObString &in, int64_t &pos, ObString &value)
{
  int ret = OB_SUCCESS;
  int64_t len = 0;
  bool has_digit = false;
  while (pos < in.length() && is_digit(in[pos])) {
    len = len * 10 + (in[pos] - '0');
    ++pos;
    has_digit = true;
  }
  if (!has_digit || pos >= in.length() || ':' != in[pos] || pos + 1 + len > in.length()) {
    ret = OB_INVALID_DATA;
  } else {
    value.assign_ptr(in.ptr() + pos + 1, static_cast<int32_t>(len));
    pos += 1 + len;
  }
  return ret;
}

bool CsvExportOptions::is_copy_properties(const ObString &properties)
{
  return properties.prefix_match(FILE_SQL_COPY_PREFIX);
}

int CsvExportOptions::encode(ObIAllocator &alloc, ObString &out) const
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  char d = delimiter_;
  char q = quote_;
  char e = escape_;
  if (OB_FAIL(buf.append(FILE_SQL_COPY_PREFIX))) {
  } else if (OB_FAIL(append_copy_field(buf, ObString(0 == d ? 0 : 1, &d)))) {
  } else if (OB_FAIL(append_copy_field(buf, ObString(0 == q ? 0 : 1, &q)))) {
  } else if (OB_FAIL(append_copy_field(buf, ObString(0 == e ? 0 : 1, &e)))) {
  } else if (OB_FAIL(append_copy_field(buf, ObString(header_ ? "1" : "0")))) {
  } else if (OB_FAIL(append_copy_field(buf, null_str_))) {
  } else if (OB_FAIL(buf.append_fmt("%ld;", column_names_.count()))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_names_.count(); ++i) {
    ret = append_copy_field(buf, column_names_.at(i));
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc, buf.string(), out))) {
  }
  return ret;
}

int CsvExportOptions::decode(const ObString &in)
{
  int ret = OB_SUCCESS;
  int64_t pos = strlen(FILE_SQL_COPY_PREFIX);
  ObString d;
  ObString q;
  ObString e;
  ObString h;
  int64_t count = 0;
  column_names_.reuse();
  if (!is_copy_properties(in)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(read_copy_field(in, pos, d)) || OB_FAIL(read_copy_field(in, pos, q))
             || OB_FAIL(read_copy_field(in, pos, e)) || OB_FAIL(read_copy_field(in, pos, h))
             || OB_FAIL(read_copy_field(in, pos, null_str_))) {
  } else {
    while (pos < in.length() && is_digit(in[pos])) {
      count = count * 10 + (in[pos] - '0');
      ++pos;
    }
    if (pos >= in.length() || ';' != in[pos]) {
      ret = OB_INVALID_DATA;
    } else {
      ++pos;
      delimiter_ = d.empty() ? 0 : d[0];
      quote_ = q.empty() ? 0 : q[0];
      escape_ = e.empty() ? 0 : e[0];
      header_ = h == "1";
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      ObString name;
      if (OB_FAIL(read_copy_field(in, pos, name))) {
      } else if (OB_FAIL(column_names_.push_back(name))) {
      }
    }
  }
  return ret;
}

CsvFileExporter::CsvFileExporter()
  : fd_(-1), options_(), buf_(NULL), buf_len_(0), buf_pos_(0), row_count_(0), finished_(false)
{
  target_path_[0] = '\0';
  tmp_path_[0] = '\0';
}

int CsvFileExporter::open(const ObString &target_real_path, const CsvExportOptions &options)
{
  int ret = OB_SUCCESS;
  static int64_t tmp_seq = 0;
  struct stat st;
  abort();
  finished_ = false;
  row_count_ = 0;
  if (target_real_path.empty() || target_real_path.length() + 64 >= static_cast<int64_t>(sizeof(target_path_))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "COPY TO path");
  } else if (OB_FAIL(options_.column_names_.assign(options.column_names_))) {
  } else {
    options_.delimiter_ = options.delimiter_;
    options_.quote_ = options.quote_;
    options_.escape_ = options.escape_;
    options_.header_ = options.header_;
    options_.null_str_ = options.null_str_;
    MEMCPY(target_path_, target_real_path.ptr(), target_real_path.length());
    target_path_[target_real_path.length()] = '\0';
    const char *slash = strrchr(target_path_, '/');
    const int64_t dir_len = NULL == slash ? 0 : slash - target_path_;
    const char *base = NULL == slash ? target_path_ : slash + 1;
    snprintf(tmp_path_, sizeof(tmp_path_), "%.*s%s.%s.tmp_%d_%ld", static_cast<int>(dir_len), target_path_,
             NULL == slash ? "" : "/", base, static_cast<int>(getpid()), ATOMIC_AAF(&tmp_seq, 1));
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    if (0 == ::lstat(target_path_, &st)) {
      ret = OB_FILE_ALREADY_EXIST;
      FileSqlUtil::set_user_error(ret, "COPY: target file '%s' already exists", target_path_);
      tmp_path_[0] = '\0';
    } else if ((fd_ = ::open(tmp_path_, flags, 0644)) < 0) {
      ret = OB_IO_ERROR;
      FileSqlUtil::set_user_error(ret, "COPY: cannot create temporary file next to '%s' (errno %d)",
                                  target_path_, errno);
      tmp_path_[0] = '\0';
    } else {
      buf_len_ = 1L << 20;
      buf_pos_ = 0;
      if (OB_ISNULL(buf_ = static_cast<char *>(ob_malloc(buf_len_, ObMemAttr("FileSqlCopy"))))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
  }
  if (OB_FAIL(ret)) {
    abort();
  }
  return ret;
}

int CsvFileExporter::flush()
{
  int ret = OB_SUCCESS;
  int64_t written = 0;
  while (OB_SUCC(ret) && written < buf_pos_) {
    const ssize_t n = ::write(fd_, buf_ + written, buf_pos_ - written);
    if (n < 0) {
      if (EINTR != errno) {
        ret = OB_IO_ERROR;
        FileSqlUtil::set_user_error(ret, "COPY: write failed (errno %d)", errno);
      }
    } else {
      written += n;
    }
  }
  buf_pos_ = 0;
  return ret;
}

int CsvFileExporter::append(const char *ptr, const int64_t len)
{
  int ret = OB_SUCCESS;
  if (fd_ < 0 || OB_ISNULL(buf_)) {
    ret = OB_NOT_INIT;
  } else if (buf_pos_ + len > buf_len_ && OB_FAIL(flush())) {
  } else if (len > buf_len_) {
    int64_t written = 0;
    while (OB_SUCC(ret) && written < len) {
      const ssize_t n = ::write(fd_, ptr + written, len - written);
      if (n < 0) {
        if (EINTR != errno) {
          ret = OB_IO_ERROR;
        }
      } else {
        written += n;
      }
    }
  } else {
    MEMCPY(buf_ + buf_pos_, ptr, len);
    buf_pos_ += len;
  }
  return ret;
}

bool CsvFileExporter::need_quote(const char *ptr, const int64_t len) const
{
  bool need = false;
  if (0 == len) {
    // read_csv reads an unquoted empty field as NULL whatever the NULL text is
    need = true;
  } else if (len == options_.null_str_.length() && 0 == MEMCMP(ptr, options_.null_str_.ptr(), len)) {
    need = true;
  } else {
    for (int64_t i = 0; !need && i < len; ++i) {
      const char c = ptr[i];
      need = c == options_.delimiter_ || c == options_.quote_ || '\n' == c || '\r' == c
             || (0 != options_.escape_ && c == options_.escape_);
    }
  }
  return need && 0 != options_.quote_;
}

int CsvFileExporter::write_field(const char *ptr, const int64_t len, const bool is_null,
                                 const bool is_first)
{
  int ret = OB_SUCCESS;
  if (!is_first && OB_FAIL(append(&options_.delimiter_, 1))) {
  } else if (is_null) {
    ret = append(options_.null_str_.ptr(), options_.null_str_.length());
  } else if (!need_quote(ptr, len)) {
    ret = append(ptr, len);
  } else if (OB_FAIL(append(&options_.quote_, 1))) {
  } else {
    const char escape = 0 == options_.escape_ ? options_.quote_ : options_.escape_;
    int64_t start = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
      if (ptr[i] == options_.quote_ || (ptr[i] == escape && escape != options_.quote_)) {
        if (OB_FAIL(append(ptr + start, i - start))) {
        } else if (OB_FAIL(append(&escape, 1))) {
        } else {
          start = i;
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(append(ptr + start, len - start))) {
    } else if (OB_FAIL(append(&options_.quote_, 1))) {
    }
  }
  return ret;
}

int CsvFileExporter::end_row()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append("\n", 1))) {
  } else {
    ++row_count_;
  }
  return ret;
}

int CsvFileExporter::write_header()
{
  int ret = OB_SUCCESS;
  if (options_.header_) {
    for (int64_t i = 0; OB_SUCC(ret) && i < options_.column_names_.count(); ++i) {
      const ObString &name = options_.column_names_.at(i);
      ret = write_field(name.ptr(), name.length(), false, 0 == i);
    }
    if (OB_SUCC(ret) && OB_FAIL(append("\n", 1))) {
    }
  }
  return ret;
}

int CsvFileExporter::finish()
{
  int ret = OB_SUCCESS;
  if (fd_ < 0) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(flush())) {
  } else if (0 != ::fsync(fd_)) {
    ret = OB_IO_ERROR;
    FileSqlUtil::set_user_error(ret, "COPY: fsync failed (errno %d)", errno);
  } else {
    ::close(fd_);
    fd_ = -1;
    // link() never replaces an existing target
    if (0 != ::link(tmp_path_, target_path_)) {
      if (EEXIST == errno) {
        ret = OB_FILE_ALREADY_EXIST;
        FileSqlUtil::set_user_error(ret, "COPY: target file '%s' already exists", target_path_);
      } else {
        ret = OB_IO_ERROR;
        FileSqlUtil::set_user_error(ret, "COPY: cannot create '%s' (errno %d)", target_path_, errno);
      }
    } else {
      finished_ = true;
    }
  }
  abort();
  return ret;
}

void CsvFileExporter::abort()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if ('\0' != tmp_path_[0]) {
    ::unlink(tmp_path_);
    tmp_path_[0] = '\0';
  }
  if (NULL != buf_) {
    ob_free(buf_);
    buf_ = NULL;
  }
  buf_pos_ = 0;
}

// ---------------------------------------------------------------------------------------
// runtime settings
// ---------------------------------------------------------------------------------------

int FileSqlUtil::check_enabled(const ObSQLSessionInfo &session)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    LOG_WARN("failed to get enable_file_sql", K(ret));
  } else if (!enabled) {
    ret = set_user_error(OB_NOT_SUPPORTED, "file sql is disabled, set enable_file_sql = ON to use "
                                           "read_csv/read_json/read_parquet/glob and COPY ... TO");
  }
  return ret;
}

int FileSqlUtil::get_secure_file_priv(ObIAllocator &alloc, ObString &secure_file_priv)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(share::schema::ObSchemaUtils::get_runtime_varchar_variable(
                 *GCTX.schema_service_, share::SYS_VAR_SECURE_FILE_PRIV, alloc, secure_file_priv))) {
    LOG_WARN("failed to get secure_file_priv", K(ret));
  }
  return ret;
}

// ---------------------------------------------------------------------------------------
// row sources
// ---------------------------------------------------------------------------------------

static int copy_varchar(ObIAllocator &alloc, FileValue &value)
{
  int ret = OB_SUCCESS;
  if (!value.is_null_ && !value.str_.empty()) {
    ObString copied;
    if (OB_FAIL(ob_write_string(alloc, value.str_, copied))) {
    } else {
      value.str_ = copied;
    }
  }
  return ret;
}

int CsvRowSource::open(const FileTableDesc &desc, const ObIArray<int64_t> &column_idxs,
                       const ObString &secure_file_priv)
{
  int ret = OB_SUCCESS;
  UNUSED(secure_file_priv);
  close();
  desc_ = &desc;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_idxs.count(); ++i) {
    if (column_idxs.at(i) < 0 || column_idxs.at(i) >= desc.get_column_count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid column index", K(ret), K(i), K(column_idxs.at(i)));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(column_idxs_.assign(column_idxs))) {
  } else if (OB_FAIL(file_.open(desc.real_path_, &desc.identity_))) {
  } else if (OB_FAIL(reader_.init(file_, desc))) {
  } else if (OB_FAIL(skip_leading_records())) {
  }
  return ret;
}

int CsvRowSource::skip_leading_records()
{
  int ret = OB_SUCCESS;
  const int64_t skip = desc_->skip_rows_ + (desc_->has_header_ ? 1 : 0);
  int64_t line_no = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < skip; ++i) {
    if (OB_FAIL(reader_.next_record(fields_, line_no))) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        break;
      }
    }
  }
  return ret;
}

int CsvRowSource::next_row(ObIAllocator &alloc, FileValue *values)
{
  int ret = OB_SUCCESS;
  bool got = false;
  const int64_t ncols = desc_->get_column_count();
  while (OB_SUCC(ret) && !got) {
    int64_t line_no = 0;
    if (OB_FAIL(reader_.next_record(fields_, line_no))) {
      if (OB_ITER_END == ret) {
        // the whole file was read, it must not have changed during the scan
        const int tmp_ret = file_.check_unchanged();
        ret = OB_SUCCESS == tmp_ret ? OB_ITER_END : tmp_ret;
      }
    } else if (fields_.count() != ncols || (desc_->ignore_errors_ && has_invalid_utf8(fields_))) {
      if (!desc_->ignore_errors_) {
        ret = FileSqlUtil::set_user_error(OB_ERR_WRONG_VALUE_COUNT_ON_ROW,
            "read_csv: bad row at line %ld of '%.*s': expected %ld fields but found %ld",
            line_no, desc_->path_.length(), desc_->path_.ptr(), ncols, fields_.count());
      }
    } else {
      got = true;
      for (int64_t k = 0; OB_SUCC(ret) && k < column_idxs_.count(); ++k) {
        const FileColumnDesc &col = desc_->columns_.at(column_idxs_.at(k));
        const CsvField &f = fields_.at(col.source_idx_);
        FileValue &value = values[k];
        int64_t chars = 0;
        value.reset();
        if (is_csv_null(*desc_, f)) {
          // value stays NULL
        } else if (FileColumnType::VARCHAR == col.get_type()
                   && OB_FAIL(check_csv_field(*desc_, f, line_no, col.source_idx_, chars))) {
        } else if (!FileSqlUtil::convert_text(col.get_type(), f.ptr_, f.len_, value)) {
          ret = FileSqlUtil::set_user_error(OB_INVALID_DATA,
              "read_csv: value '%.*s' at line %ld, column '%.*s' of '%.*s' is not a valid %s",
              static_cast<int>(std::min<int64_t>(f.len_, 64)), f.ptr_, line_no, col.name_.length(),
              col.name_.ptr(), desc_->path_.length(), desc_->path_.ptr(),
              file_column_type_name(col.get_type()));
        } else if (FileColumnType::VARCHAR == col.get_type() && OB_FAIL(copy_varchar(alloc, value))) {
        }
      }
    }
  }
  return ret;
}

int CsvRowSource::rewind()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(desc_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(reader_.rewind())) {
  } else if (OB_FAIL(skip_leading_records())) {
  }
  return ret;
}

void CsvRowSource::close()
{
  reader_.destroy();
  file_.close();
}

int JsonRowSource::open(const FileTableDesc &desc, const ObIArray<int64_t> &column_idxs,
                        const ObString &secure_file_priv)
{
  int ret = OB_SUCCESS;
  UNUSED(secure_file_priv);
  close();
  desc_ = &desc;
  if (OB_FAIL(column_idxs_.assign(column_idxs))) {
  } else if (OB_FAIL(key_map_.create(std::max<int64_t>(column_idxs.count() * 2, 16), "FileSqlJsonKey"))) {
  }
  for (int64_t k = 0; OB_SUCC(ret) && k < column_idxs.count(); ++k) {
    if (column_idxs.at(k) < 0 || column_idxs.at(k) >= desc.get_column_count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid column index", K(ret), K(k), K(column_idxs.at(k)));
    } else if (OB_FAIL(key_map_.set_refactored(desc.columns_.at(column_idxs.at(k)).source_name_, k))) {
      LOG_WARN("failed to add json key", K(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(file_.open(desc.real_path_, &desc.identity_))) {
  } else if (OB_FAIL(reader_.init(file_, desc.max_object_size_))) {
  }
  return ret;
}

int JsonRowSource::next_row(ObIAllocator &alloc, FileValue *values)
{
  int ret = OB_SUCCESS;
  bool got = false;
  while (OB_SUCC(ret) && !got) {
    const char *line = NULL;
    int64_t len = 0;
    int64_t line_no = 0;
    int64_t pos = 0;
    if (OB_FAIL(reader_.next_line(line, len, line_no))) {
      if (OB_ITER_END == ret) {
        // the whole file was read, it must not have changed during the scan
        const int tmp_ret = file_.check_unchanged();
        ret = OB_SUCCESS == tmp_ret ? OB_ITER_END : tmp_ret;
      }
    } else if (FALSE_IT(skip_json_ws(line, len, pos))) {
    } else if (pos >= len) {
      // blank line
    } else if (OB_FAIL(parser_.parse(line, len, line_no, fields_))) {
      if (desc_->ignore_errors_
          && (OB_ERR_INVALID_JSON_TEXT == ret || OB_ERR_INVALID_CHARACTER_STRING == ret)) {
        ret = OB_SUCCESS;
      }
    } else {
      got = true;
      for (int64_t k = 0; k < column_idxs_.count(); ++k) {
        values[k].reset();
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < fields_.count(); ++i) {
        const JsonField &f = fields_.at(i);
        int64_t k = -1;
        int tmp_ret = key_map_.get_refactored(f.key_, k);
        if (OB_HASH_NOT_EXIST == tmp_ret) {
          // not projected
        } else if (OB_SUCCESS != tmp_ret) {
          ret = tmp_ret;
        } else {
          const FileColumnDesc &col = desc_->columns_.at(column_idxs_.at(k));
          if (!JsonLineParser::convert_json(col.get_type(), f, values[k])) {
            ret = FileSqlUtil::set_user_error(OB_INVALID_DATA,
                "read_json: value of field '%.*s' at line %ld of '%.*s' is not a valid %s",
                f.key_.length(), f.key_.ptr(), line_no, desc_->path_.length(),
                desc_->path_.ptr(), file_column_type_name(col.get_type()));
          } else if (FileColumnType::VARCHAR == col.get_type()
                     && OB_FAIL(copy_varchar(alloc, values[k]))) {
          }
        }
      }
    }
  }
  return ret;
}

int JsonRowSource::rewind()
{
  return reader_.rewind();
}

void JsonRowSource::close()
{
  reader_.destroy();
  file_.close();
  if (key_map_.created()) {
    key_map_.destroy();
  }
}

int GlobRowSource::open(const FileTableDesc &desc, const ObIArray<int64_t> &column_idxs,
                        const ObString &secure_file_priv)
{
  int ret = OB_SUCCESS;
  close();
  column_count_ = column_idxs.count();
  if (OB_FAIL(FileGlob::glob(desc.path_, secure_file_priv, alloc_, files_))) {
  }
  return ret;
}

int GlobRowSource::next_row(ObIAllocator &alloc, FileValue *values)
{
  int ret = OB_SUCCESS;
  if (idx_ >= files_.count()) {
    ret = OB_ITER_END;
  } else {
    for (int64_t k = 0; OB_SUCC(ret) && k < column_count_; ++k) {
      values[k].reset();
      values[k].is_null_ = false;
      values[k].str_ = files_.at(idx_);
      ret = copy_varchar(alloc, values[k]);
    }
    ++idx_;
  }
  return ret;
}

void GlobRowSource::close()
{
  files_.reset();
  alloc_.reset();
  idx_ = 0;
}

} // namespace sql
} // namespace oceanbase
