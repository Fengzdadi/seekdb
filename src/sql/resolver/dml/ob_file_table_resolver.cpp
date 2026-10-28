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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_file_table_resolver.h"

#include "share/schema/ob_schema_utils.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/engine/basic/ob_file_parquet_reader.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/resolver/ob_resolver_define.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{

namespace
{
inline ObString node_str(const ParseNode *node)
{
  return NULL == node || NULL == node->str_value_
         ? ObString()
         : ObString(static_cast<int32_t>(node->str_len_), node->str_value_);
}

bool is_string_node(const ParseNode *node)
{
  return NULL != node && (T_VARCHAR == node->type_ || T_CHAR == node->type_
                          || T_NCHAR == node->type_ || T_NVARCHAR2 == node->type_
                          || T_IDENT == node->type_);
}

int option_error(const char *func, const ObString &name)
{
  int ret = OB_INVALID_ARGUMENT;
  FileSqlUtil::set_user_error(ret, "Incorrect value for %s option '%.*s'", func, name.length(),
                              name.ptr());
  return ret;
}

int option_not_supported(const char *func, const ObString &name, const char *detail)
{
  int ret = OB_NOT_SUPPORTED;
  FileSqlUtil::set_user_error(ret, "%s option '%.*s'%s not supported", func, name.length(),
                              name.ptr(), detail);
  return ret;
}

int get_bool_option(const char *func, const ObString &name, const ParseNode *value, bool &v)
{
  int ret = OB_SUCCESS;
  if (NULL == value) {
    v = true;
  } else if (T_BOOL == value->type_ || T_INT == value->type_) {
    if (0 != value->value_ && 1 != value->value_) {
      ret = option_error(func, name);
    } else {
      v = 1 == value->value_;
    }
  } else if (is_string_node(value)) {
    const ObString s = node_str(value);
    if (0 == s.case_compare("true") || 0 == s.case_compare("on") || s == "1") {
      v = true;
    } else if (0 == s.case_compare("false") || 0 == s.case_compare("off") || s == "0") {
      v = false;
    } else {
      ret = option_error(func, name);
    }
  } else {
    ret = option_error(func, name);
  }
  return ret;
}

int get_int_option(const char *func, const ObString &name, const ParseNode *value, int64_t &v)
{
  int ret = OB_SUCCESS;
  if (NULL != value && T_INT == value->type_) {
    v = value->value_;
  } else if (is_string_node(value)) {
    const ObString s = node_str(value);
    if (!FileSqlUtil::parse_bigint(s.ptr(), s.length(), v)) {
      ret = option_error(func, name);
    }
  } else {
    ret = option_error(func, name);
  }
  return ret;
}

int get_string_option(const char *func, const ObString &name, const ParseNode *value,
                      ObIAllocator &alloc, ObString &v)
{
  int ret = OB_SUCCESS;
  if (!is_string_node(value)) {
    ret = option_error(func, name);
  } else if (OB_FAIL(ob_write_string(alloc, node_str(value), v))) {
  }
  return ret;
}

// A single byte; '' means none when allow_empty. The two character spelling "\t" is a tab.
int get_char_option(const char *func, const ObString &name, const ParseNode *value,
                    const bool allow_empty, int8_t &v)
{
  int ret = OB_SUCCESS;
  if (!is_string_node(value) || T_IDENT == value->type_) {
    ret = option_error(func, name);
  } else {
    const ObString s = node_str(value);
    if (0 == s.length() && allow_empty) {
      v = 0;
    } else if (1 == s.length() && '\n' != s[0] && '\r' != s[0]) {
      v = static_cast<int8_t>(s[0]);
    } else if (2 == s.length() && '\\' == s[0] && 't' == s[1]) {
      v = '\t';
    } else {
      ret = option_error(func, name);
    }
  }
  return ret;
}

bool name_is(const ObString &name, const char *option)
{
  return 0 == name.case_compare(option);
}
} // anonymous namespace

bool FileTableResolver::is_file_table_func(const ObString &name, FileFormat &format)
{
  format = FileFormat::INVALID;
  if (name_is(name, "read_csv") || name_is(name, "read_csv_auto")) {
    format = FileFormat::CSV;
  } else if (name_is(name, "read_json") || name_is(name, "read_json_auto")
             || name_is(name, "read_ndjson") || name_is(name, "read_ndjson_auto")) {
    format = FileFormat::JSON;
  } else if (name_is(name, "read_parquet") || name_is(name, "parquet_scan")) {
    format = FileFormat::PARQUET;
  } else if (name_is(name, "glob")) {
    format = FileFormat::GLOB;
  }
  return FileFormat::INVALID != format;
}

int FileTableResolver::check_enabled(const ObSQLSessionInfo &session)
{
  return FileSqlUtil::check_enabled(session);
}

int FileTableResolver::get_secure_file_priv(ObIAllocator &alloc, ObString &secure_file_priv)
{
  return FileSqlUtil::get_secure_file_priv(alloc, secure_file_priv);
}

void FileTableResolver::disable_plan_cache(ObQueryCtx *query_ctx)
{
  if (NULL != query_ctx) {
    ObGlobalHint &global_hint =
        const_cast<ObGlobalHint &>(query_ctx->get_query_hint_for_update().get_global_hint());
    global_hint.merge_plan_cache_hint(OB_USE_PLAN_CACHE_NONE);
  }
}

int FileTableResolver::resolve_options(const ParseNode *option_list, ObIAllocator &alloc,
                                       FileTableDesc &desc)
{
  int ret = OB_SUCCESS;
  const FileFormat format = desc.get_format();
  ObSqlString func_buf;
  if (OB_FAIL(func_buf.append(desc.func_name_))) {
  }
  const char *func = func_buf.empty() ? "file sql" : func_buf.ptr();
  bool delimiter_set = false;
  for (int64_t i = 0; OB_SUCC(ret) && NULL != option_list && i < option_list->num_child_; ++i) {
    const ParseNode *opt = option_list->children_[i];
    if (OB_ISNULL(opt) || T_FILE_TABLE_OPTION != opt->type_ || 2 != opt->num_child_
        || OB_ISNULL(opt->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid file table option", K(ret));
    } else {
      const ObString name = node_str(opt->children_[0]);
      const ParseNode *value = opt->children_[1];
      bool b = false;
      int64_t n = 0;
      ObString s;
      if (name_is(name, "sample_size")) {
        // schema inference always reads the whole file
        if (OB_FAIL(get_int_option(func, name, value, n))) {
        } else if (-1 != n) {
          ret = option_not_supported(func, name, " other than -1 (full scan inference) is");
        }
      } else if (name_is(name, "auto_detect")) {
        if (OB_FAIL(get_bool_option(func, name, value, b))) {
        } else if (!b) {
          ret = option_not_supported(func, name, " = false is");
        }
      } else if (name_is(name, "compression")) {
        if (OB_FAIL(get_string_option(func, name, value, alloc, s))) {
        } else if (!(name_is(s, "none") || name_is(s, "auto") || name_is(s, "uncompressed"))) {
          ret = option_not_supported(func, name, " other than 'none' is");
        }
      } else if (name_is(name, "ignore_errors") && FileFormat::PARQUET != format
                 && FileFormat::GLOB != format) {
        if (OB_FAIL(get_bool_option(func, name, value, b))) {
        } else {
          desc.ignore_errors_ = b;
        }
      } else if (FileFormat::CSV == format) {
        if (name_is(name, "header")) {
          if (OB_FAIL(get_bool_option(func, name, value, b))) {
          } else {
            desc.header_option_ = b ? 1 : 0;
          }
        } else if (name_is(name, "delim") || name_is(name, "delimiter") || name_is(name, "sep")
                   || name_is(name, "separator")) {
          if (OB_FAIL(get_char_option(func, name, value, false, desc.delimiter_))) {
          } else {
            delimiter_set = true;
          }
        } else if (name_is(name, "quote")) {
          if (OB_FAIL(get_char_option(func, name, value, true, desc.quote_))) {
          }
        } else if (name_is(name, "escape")) {
          if (OB_FAIL(get_char_option(func, name, value, true, desc.escape_))) {
          }
        } else if (name_is(name, "nullstr") || name_is(name, "null_str")) {
          if (OB_FAIL(get_string_option(func, name, value, alloc, desc.null_str_))) {
          }
        } else if (name_is(name, "all_varchar")) {
          if (OB_FAIL(get_bool_option(func, name, value, b))) {
          } else {
            desc.all_varchar_ = b;
          }
        } else if (name_is(name, "skip")) {
          if (OB_FAIL(get_int_option(func, name, value, n))) {
          } else if (n < 0) {
            ret = option_error(func, name);
          } else {
            desc.skip_rows_ = n;
          }
        } else if (name_is(name, "max_line_size") || name_is(name, "maximum_line_size")) {
          if (OB_FAIL(get_int_option(func, name, value, n))) {
          } else if (n <= 0 || n > FileSqlLimits::MAX_LINE_SIZE_LIMIT) {
            ret = option_error(func, name);
          } else {
            desc.max_line_size_ = n;
          }
        } else if (name_is(name, "new_line") || name_is(name, "newline")) {
          // LF, CRLF and CR are always recognized
          if (OB_FAIL(get_string_option(func, name, value, alloc, s))) {
          }
        } else if (name_is(name, "encoding")) {
          if (OB_FAIL(get_string_option(func, name, value, alloc, s))) {
          } else if (!(name_is(s, "utf-8") || name_is(s, "utf8"))) {
            ret = option_not_supported(func, name, " other than 'utf-8' is");
          }
        } else {
          ret = option_not_supported(func, name, " is");
        }
      } else if (FileFormat::JSON == format) {
        if (name_is(name, "format")) {
          if (OB_FAIL(get_string_option(func, name, value, alloc, s))) {
          } else if (name_is(s, "newline_delimited") || name_is(s, "nd")
                     || name_is(s, "auto")) {
          } else if (name_is(s, "array") || name_is(s, "unstructured")) {
            ret = option_not_supported(func, name, " other than 'newline_delimited' is");
          } else {
            ret = option_error(func, name);
          }
        } else if (name_is(name, "records")) {
          if (NULL != value && is_string_node(value) && name_is(node_str(value), "auto")) {
          } else if (OB_FAIL(get_bool_option(func, name, value, b))) {
          } else if (!b) {
            ret = option_not_supported(func, name, " = false is");
          }
        } else if (name_is(name, "maximum_object_size")) {
          if (OB_FAIL(get_int_option(func, name, value, n))) {
          } else if (n <= 0 || n > FileSqlLimits::MAX_LINE_SIZE_LIMIT) {
            ret = option_error(func, name);
          } else {
            desc.max_object_size_ = n;
          }
        } else {
          ret = option_not_supported(func, name, " is");
        }
      } else if (FileFormat::PARQUET == format) {
        if (name_is(name, "binary_as_string")) {
          if (OB_FAIL(get_bool_option(func, name, value, b))) {
          }
        } else {
          ret = option_not_supported(func, name, " is");
        }
      } else {
        ret = option_not_supported(func, name, " is");
      }
    }
  }
  if (OB_SUCC(ret) && FileFormat::CSV == format) {
    const ObString &path = desc.path_;
    if (delimiter_set) {
    } else if (path.suffix_match_ci(".tsv") || path.suffix_match_ci(".tab")) {
      desc.delimiter_ = '\t';
    } else {
      // detected by the inference, see sniff_csv_delimiter
      desc.delimiter_ = 0;
    }
    if (0 == desc.quote_) {
      // without quoting there is nothing to escape
      desc.escape_ = 0;
    }
    if (0 != desc.quote_ && desc.quote_ == desc.delimiter_) {
      ret = FileSqlUtil::set_user_error(OB_INVALID_ARGUMENT,
                                        "%s: quote and delimiter must be different", func);
    } else if (0 != desc.escape_ && desc.escape_ == desc.delimiter_) {
      ret = FileSqlUtil::set_user_error(OB_INVALID_ARGUMENT,
                                        "%s: escape and delimiter must be different", func);
    }
  }
  return ret;
}

int FileTableResolver::resolve_file_table(const ParseNode &node, const ObSQLSessionInfo &session,
                                          ObIAllocator &alloc, FileTableDesc *&desc,
                                          ObString &alias_name)
{
  int ret = OB_SUCCESS;
  FileFormat format = FileFormat::INVALID;
  ObString func_name;
  ObString path;
  ObString secure_file_priv;
  void *buf = NULL;
  desc = NULL;
  if (OB_UNLIKELY(T_FILE_TABLE_EXPRESSION != node.type_ || 4 != node.num_child_)
      || OB_ISNULL(node.children_[0]) || OB_ISNULL(node.children_[1])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid file table node", K(ret), K(node.type_), K(node.num_child_));
  } else if (OB_FAIL(check_enabled(session))) {
  } else if (!is_file_table_func(node_str(node.children_[0]), format)) {
    ret = OB_ERR_PARSER_SYNTAX;
    LOG_WARN("unknown table function", K(ret));
  } else if (OB_FAIL(ob_write_string(alloc, node_str(node.children_[0]), func_name))) {
  } else if (OB_FAIL(ob_write_string(alloc, node_str(node.children_[1]), path, true))) {
  } else if (OB_ISNULL(buf = alloc.alloc(sizeof(FileTableDesc)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    desc = new (buf) FileTableDesc(alloc);
    // keep the function name in lower case, as written in the DuckDB documentation
    for (int64_t i = 0; i < func_name.length(); ++i) {
      func_name.ptr()[i] = static_cast<char>(tolower(func_name[i]));
    }
    desc->set_format(format);
    desc->func_name_ = func_name;
    desc->path_ = path;
    if (NULL != node.children_[3] && node.children_[3]->str_len_ > 0) {
      if (OB_FAIL(ob_write_string(alloc, node_str(node.children_[3]), alias_name))) {
      }
    } else {
      alias_name = func_name;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(resolve_options(node.children_[2], alloc, *desc))) {
  } else if (OB_FAIL(get_secure_file_priv(alloc, secure_file_priv))) {
  } else if (FileFormat::GLOB == format) {
    desc->real_path_ = path;
    if (OB_FAIL(FileSchemaInferrer::infer_glob(alloc, secure_file_priv, *desc))) {
      LOG_WARN("failed to glob", K(ret), K(path));
    }
  } else {
    LocalFileReader file;
    if (OB_FAIL(FileSqlUtil::resolve_input_path(path, secure_file_priv, alloc, desc->real_path_))) {
      LOG_WARN("invalid input path", K(ret), K(path));
    } else if (OB_FAIL(FileSqlUtil::check_extension(format, path))) {
    } else if (OB_FAIL(file.open(desc->real_path_, NULL))) {
      LOG_WARN("failed to open file", K(ret), K(path));
    } else {
      desc->identity_ = file.get_identity();
      file.close();
      if (FileFormat::CSV == format) {
        ret = FileSchemaInferrer::infer_csv(alloc, *desc);
      } else if (FileFormat::JSON == format) {
        ret = FileSchemaInferrer::infer_json(alloc, *desc);
      } else {
        ret = infer_parquet_schema(alloc, *desc);
      }
      if (OB_FAIL(ret)) {
        LOG_WARN("failed to infer file schema", K(ret), K(path));
      }
    }
  }
  if (OB_SUCC(ret) && desc->get_column_count() <= 0) {
    ret = FileSqlUtil::set_user_error(OB_INVALID_DATA,
        "%.*s: no column can be inferred from '%.*s', the file is empty", func_name.length(),
        func_name.ptr(), path.length(), path.ptr());
  }
  return ret;
}

int FileTableResolver::get_column_type(const FileColumnDesc &column,
                                       const ObSQLSessionInfo &session,
                                       ObRawExprResType &type)
{
  int ret = OB_SUCCESS;
  ObCollationType cs_type = CS_TYPE_UTF8MB4_GENERAL_CI;
  ObCollationType conn_cs = CS_TYPE_INVALID;
  if (OB_SUCCESS == session.get_collation_connection(conn_cs)
      && CHARSET_UTF8MB4 == ObCharset::charset_type_by_coll(conn_cs)) {
    cs_type = conn_cs;
  }
  switch (column.get_type()) {
    case FileColumnType::NULL_TYPE:
      type.set_null();
      break;
    case FileColumnType::BOOLEAN:
      type.set_tinyint();
      type.set_precision(1);
      type.set_scale(0);
      break;
    case FileColumnType::BIGINT:
      type.set_int();
      type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObIntType]);
      break;
    case FileColumnType::DOUBLE:
      type.set_double();
      type.set_precision(PRECISION_UNKNOWN_YET);
      type.set_scale(SCALE_UNKNOWN_YET);
      break;
    case FileColumnType::DATE:
      type.set_date();
      type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObDateType]);
      break;
    case FileColumnType::DATETIME:
      type.set_datetime();
      type.set_scale(column.scale_);
      type.set_precision(static_cast<ObPrecision>(19 + (column.scale_ > 0 ? column.scale_ + 1 : 0)));
      break;
    case FileColumnType::VARCHAR:
      type.set_varchar();
      type.set_collation_type(cs_type);
      type.set_collation_level(CS_LEVEL_IMPLICIT);
      type.set_length(static_cast<ObLength>(column.max_length_ > 0 ? column.max_length_ : 1));
      break;
    default:
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected file column type", K(ret), K(column));
      break;
  }
  if (OB_SUCC(ret) && !column.nullable_) {
    type.set_result_flag(NOT_NULL_FLAG);
  }
  return ret;
}

int FileTableResolver::resolve_copy_into(const ParseNode &node, const ObSQLSessionInfo &session,
                                         ObIAllocator &alloc, ObString &path,
                                         CsvExportOptions &options)
{
  int ret = OB_SUCCESS;
  const char *func = "COPY";
  const ParseNode *option_list = NULL;
  bool escape_set = false;
  if (OB_UNLIKELY(T_COPY_INTO != node.type_ || 2 != node.num_child_)
      || OB_ISNULL(node.children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid copy node", K(ret));
  } else if (OB_FAIL(check_enabled(session))) {
  } else if (OB_FAIL(ob_write_string(alloc, node_str(node.children_[0]), path, true))) {
  } else if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "COPY target path");
  } else {
    option_list = node.children_[1];
    // as in DuckDB, COPY ... TO writes a header line unless HEADER false is given
    options.header_ = true;
  }
  for (int64_t i = 0; OB_SUCC(ret) && NULL != option_list && i < option_list->num_child_; ++i) {
    const ParseNode *opt = option_list->children_[i];
    if (OB_ISNULL(opt) || T_COPY_OPTION != opt->type_ || 2 != opt->num_child_
        || OB_ISNULL(opt->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const ObString name = node_str(opt->children_[0]);
      const ParseNode *value = opt->children_[1];
      bool b = false;
      int8_t c = 0;
      ObString s;
      if (name_is(name, "format")) {
        if (OB_FAIL(get_string_option(func, name, value, alloc, s))) {
        } else if (!name_is(s, "csv")) {
          ret = option_not_supported(func, name, " other than csv is");
        }
      } else if (name_is(name, "header")) {
        if (OB_FAIL(get_bool_option(func, name, value, b))) {
        } else {
          options.header_ = b;
        }
      } else if (name_is(name, "delimiter") || name_is(name, "delim") || name_is(name, "sep")
                 || name_is(name, "separator")) {
        if (OB_FAIL(get_char_option(func, name, value, false, c))) {
        } else {
          options.delimiter_ = static_cast<char>(c);
        }
      } else if (name_is(name, "quote")) {
        if (OB_FAIL(get_char_option(func, name, value, true, c))) {
        } else {
          options.quote_ = static_cast<char>(c);
        }
      } else if (name_is(name, "escape")) {
        if (OB_FAIL(get_char_option(func, name, value, true, c))) {
        } else {
          options.escape_ = static_cast<char>(c);
          escape_set = true;
        }
      } else if (name_is(name, "nullstr") || name_is(name, "null")) {
        if (OB_FAIL(get_string_option(func, name, value, alloc, options.null_str_))) {
        }
      } else if (name_is(name, "compression")) {
        if (OB_FAIL(get_string_option(func, name, value, alloc, s))) {
        } else if (!(name_is(s, "none") || name_is(s, "uncompressed"))) {
          ret = option_not_supported(func, name, " other than 'none' is");
        }
      } else {
        ret = option_not_supported(func, name, " is");
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (0 != options.quote_ && options.quote_ == options.delimiter_) {
      ret = FileSqlUtil::set_user_error(OB_INVALID_ARGUMENT,
                                        "COPY: quote and delimiter must be different");
    } else if (escape_set && 0 != options.escape_ && options.escape_ == options.delimiter_) {
      ret = FileSqlUtil::set_user_error(OB_INVALID_ARGUMENT,
                                        "COPY: escape and delimiter must be different");
    } else if (!escape_set || 0 == options.escape_) {
      // like the reader, the escape character defaults to the quote character
      options.escape_ = options.quote_;
    }
  }
  return ret;
}

int FileTableResolver::finish_copy_into(ObSelectStmt &stmt, ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  ObSelectIntoItem *into_item = stmt.get_select_into();
  if (NULL != into_item && CsvExportOptions::is_copy_properties(into_item->external_properties_)) {
    CsvExportOptions options;
    if (OB_FAIL(options.decode(into_item->external_properties_))) {
      LOG_WARN("failed to decode copy options", K(ret));
    } else {
      options.column_names_.reuse();
      for (int64_t i = 0; OB_SUCC(ret) && i < stmt.get_select_item_size(); ++i) {
        if (OB_FAIL(options.column_names_.push_back(stmt.get_select_item(i).alias_name_))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(options.encode(alloc, into_item->external_properties_))) {
        LOG_WARN("failed to encode copy options", K(ret));
      }
    }
  }
  return ret;
}

namespace
{
// Type name shown by DESCRIBE for an expression that is not a file column.
void describe_type_name(const ObRawExprResType &type, ObSqlString &out)
{
  const ObObjType t = type.get_type();
  int ret = OB_SUCCESS;
  if (ObTinyIntType == t && 1 == type.get_precision()) {
    ret = out.append("BOOLEAN");
  } else if (ObTinyIntType == t) {
    ret = out.append("TINYINT");
  } else if (ObSmallIntType == t) {
    ret = out.append("SMALLINT");
  } else if (ObMediumIntType == t || ObInt32Type == t) {
    ret = out.append("INTEGER");
  } else if (ObIntType == t) {
    ret = out.append("BIGINT");
  } else if (ob_is_unsigned_type(t)) {
    ret = out.append("UBIGINT");
  } else if (ObFloatType == t || ObUFloatType == t) {
    ret = out.append("FLOAT");
  } else if (ObDoubleType == t || ObUDoubleType == t) {
    ret = out.append("DOUBLE");
  } else if (ob_is_number_tc(t) || ob_is_decimal_int_tc(t)) {
    if (type.get_precision() > 0 && type.get_scale() >= 0) {
      ret = out.append_fmt("DECIMAL(%d,%d)", type.get_precision(), type.get_scale());
    } else {
      ret = out.append("DECIMAL");
    }
  } else if (ObDateType == t || ObMySQLDateType == t) {
    ret = out.append("DATE");
  } else if (ObDateTimeType == t || ObTimestampType == t || ObMySQLDateTimeType == t) {
    ret = out.append("DATETIME");
  } else if (ObTimeType == t) {
    ret = out.append("TIME");
  } else if (ObNullType == t) {
    ret = out.append("NULL");
  } else if (ob_is_string_or_lob_type(t) && CS_TYPE_BINARY == type.get_collation_type()) {
    ret = out.append("BLOB");
  } else if (ob_is_string_or_lob_type(t)) {
    ret = out.append("VARCHAR");
  } else {
    ret = out.append(ob_obj_type_str(t));
  }
  UNUSED(ret);
}

// DESCRIBE answers with a VALUES query, so the FILE privilege a file table needs is checked here.
int check_file_priv(ObResolverParams &params)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("FileSqlPriv");
  ObSessionPrivInfo session_priv;
  ObStmtNeedPrivs stmt_need_privs(alloc);
  ObNeedPriv need_priv;
  need_priv.priv_set_ = OB_PRIV_FILE;
  need_priv.priv_level_ = OB_PRIV_USER_LEVEL;
  if (OB_ISNULL(params.schema_checker_) || OB_ISNULL(params.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(params.session_info_->get_session_priv_info(session_priv))) {
    LOG_WARN("failed to get session priv info", K(ret));
  } else if (OB_FAIL(stmt_need_privs.need_privs_.init(1))) {
  } else if (OB_FAIL(stmt_need_privs.need_privs_.push_back(need_priv))) {
  } else if (OB_FAIL(params.schema_checker_->check_priv(
                 session_priv, params.session_info_->get_enable_role_array(), stmt_need_privs))) {
    LOG_WARN("no FILE privilege", K(ret));
  }
  return ret;
}

int append_sql_literal(ObSqlString &sql, const ObString &str, const bool backslash_escapes)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
    const char c = str[i];
    if ('\'' == c) {
      ret = sql.append("''");
    } else if ('\\' == c && backslash_escapes) {
      ret = sql.append("\\\\");
    } else {
      ret = sql.append(&c, 1);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
  }
  return ret;
}
} // anonymous namespace

int FileTableResolver::resolve_describe(ObResolverParams &params, const ParseNode &parse_tree,
                                        ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSQLSessionInfo *session = params.session_info_;
  stmt = NULL;
  if (OB_UNLIKELY(T_FILE_SQL_DESCRIBE != parse_tree.type_ || 1 != parse_tree.num_child_)
      || OB_ISNULL(parse_tree.children_[0]) || OB_ISNULL(session)
      || OB_ISNULL(params.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid describe node", K(ret));
  } else if (OB_FAIL(check_enabled(*session))) {
  } else if (OB_FAIL(check_file_priv(params))) {
  } else {
    const bool backslash_escapes = 0 == (SMO_NO_BACKSLASH_ESCAPES & session->get_sql_mode());
    HEAP_VAR(ObSelectResolver, query_resolver, params) {
      ObSelectStmt *query = NULL;
      if (OB_FAIL(query_resolver.resolve(*parse_tree.children_[0]))) {
        LOG_WARN("failed to resolve described query", K(ret));
      } else if (OB_ISNULL(query = query_resolver.get_select_stmt())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(sql.append("SELECT t.column_0 AS column_name, t.column_1 AS column_type, "
                                    "t.column_2 AS `null`, NULL AS `key`, NULL AS `default`, "
                                    "NULL AS extra FROM (VALUES "))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && NULL != query && i < query->get_select_item_size(); ++i) {
        const SelectItem &item = query->get_select_item(i);
        ObSqlString type_name;
        bool nullable = true;
        const ObRawExpr *expr = item.expr_;
        if (OB_ISNULL(expr)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          const FileColumnDesc *file_col = NULL;
          if (expr->is_column_ref_expr()) {
            const ObColumnRefRawExpr *col = static_cast<const ObColumnRefRawExpr *>(expr);
            const TableItem *table = query->get_table_item_by_id(col->get_table_id());
            const int64_t idx = static_cast<int64_t>(col->get_column_id()) - OB_APP_MIN_COLUMN_ID;
            if (NULL != table && table->is_file_table() && NULL != table->file_table_def_
                && idx >= 0 && idx < table->file_table_def_->get_column_count()) {
              file_col = &table->file_table_def_->columns_.at(idx);
            }
          }
          if (NULL != file_col) {
            ret = type_name.append(file_column_type_name(file_col->get_type()));
            nullable = file_col->nullable_;
          } else {
            describe_type_name(expr->get_result_type(), type_name);
            nullable = !expr->get_result_type().has_result_flag(NOT_NULL_FLAG);
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(sql.append(0 == i ? "ROW(" : ", ROW("))) {
        } else if (OB_FAIL(append_sql_literal(sql, item.alias_name_, backslash_escapes))) {
        } else if (OB_FAIL(sql.append(", "))) {
        } else if (OB_FAIL(append_sql_literal(sql, type_name.string(), backslash_escapes))) {
        } else if (OB_FAIL(sql.append(nullable ? ", 'YES')" : ", 'NO')"))) {
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append(") t"))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    ParseResult parse_result;
    ObParser parser(*params.allocator_, session->get_sql_mode(),
                    session->get_charsets4parser());
    ObString sql_str;
    ParseNode *select_node = NULL;
    if (OB_FAIL(ob_write_string(*params.allocator_, sql.string(), sql_str))) {
    } else if (OB_FAIL(parser.parse(sql_str, parse_result))) {
      LOG_WARN("failed to parse describe query", K(ret), K(sql_str));
    } else if (OB_ISNULL(parse_result.result_tree_)
               || OB_UNLIKELY(1 != parse_result.result_tree_->num_child_)
               || OB_ISNULL(select_node = parse_result.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      HEAP_VAR(ObSelectResolver, describe_resolver, params) {
        describe_resolver.set_calc_found_rows(true);
        describe_resolver.set_has_top_limit(true);
        if (OB_FAIL(describe_resolver.resolve(*select_node))) {
          LOG_WARN("failed to resolve describe query", K(ret), K(sql_str));
        } else {
          stmt = describe_resolver.get_basic_stmt();
          disable_plan_cache(params.query_ctx_);
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
