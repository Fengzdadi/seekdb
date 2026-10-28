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

#define USING_LOG_PREFIX SHARE_SCHEMA

#include "share/schema/ob_ai_column_schema.h"
#include "lib/hash_func/murmur_hash.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/string/ob_sql_string.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
using namespace common;
namespace share
{
namespace schema
{

OB_SERIALIZE_MEMBER(AIColumnDef,
                    column_id_,
                    state_column_id_,
                    request_column_id_,
                    spec_column_id_,
                    column_name_,
                    state_column_name_,
                    request_column_name_,
                    spec_column_name_,
                    function_type_,
                    model_name_,
                    expr_str_,
                    prompt_expr_,
                    normalized_expr_,
                    type_prompt_,
                    config_,
                    dependency_column_ids_,
                    dependency_column_names_,
                    result_type_,
                    vector_dimension_,
                    max_retries_,
                    definition_version_);

OB_SERIALIZE_MEMBER(AIColumnMeta, defs_);

void AIColumnDef::reset()
{
  column_id_ = OB_INVALID_ID;
  state_column_id_ = OB_INVALID_ID;
  request_column_id_ = OB_INVALID_ID;
  spec_column_id_ = OB_INVALID_ID;
  column_name_.reset();
  state_column_name_.reset();
  request_column_name_.reset();
  spec_column_name_.reset();
  function_type_ = AI_COLUMN_FUNC_INVALID;
  model_name_.reset();
  expr_str_.reset();
  prompt_expr_.reset();
  normalized_expr_.reset();
  type_prompt_.reset();
  config_.reset();
  dependency_column_ids_.reset();
  dependency_column_names_.reset();
  result_type_.reset();
  vector_dimension_ = 0;
  max_retries_ = 0;
  definition_version_ = 1;
}

bool AIColumnDef::is_valid() const
{
  return OB_INVALID_ID != column_id_
      && OB_INVALID_ID != state_column_id_
      && OB_INVALID_ID != request_column_id_
      && OB_INVALID_ID != spec_column_id_
      && AI_COLUMN_FUNC_INVALID != function_type_
      && !model_name_.empty()
      && !prompt_expr_.empty();
}

bool AIColumnDef::depends_on(const uint64_t column_id) const
{
  bool found = false;
  for (int64_t i = 0; !found && i < dependency_column_ids_.count(); ++i) {
    found = (column_id == dependency_column_ids_.at(i));
  }
  return found;
}

const char *AIColumnDef::get_function_name() const
{
  const char *name = "UNKNOWN";
  if (is_complete()) {
    name = "AI_COMPLETE";
  } else if (is_embed()) {
    name = "AI_EMBED";
  }
  return name;
}

int AIColumnDef::assign(const AIColumnDef &other)
{
  int ret = OB_SUCCESS;
  if (this != &other) {
    column_id_ = other.column_id_;
    state_column_id_ = other.state_column_id_;
    request_column_id_ = other.request_column_id_;
    spec_column_id_ = other.spec_column_id_;
    column_name_ = other.column_name_;
    state_column_name_ = other.state_column_name_;
    request_column_name_ = other.request_column_name_;
    spec_column_name_ = other.spec_column_name_;
    function_type_ = other.function_type_;
    model_name_ = other.model_name_;
    expr_str_ = other.expr_str_;
    prompt_expr_ = other.prompt_expr_;
    normalized_expr_ = other.normalized_expr_;
    type_prompt_ = other.type_prompt_;
    config_ = other.config_;
    result_type_ = other.result_type_;
    vector_dimension_ = other.vector_dimension_;
    max_retries_ = other.max_retries_;
    definition_version_ = other.definition_version_;
    if (OB_FAIL(dependency_column_ids_.assign(other.dependency_column_ids_))) {
    } else if (OB_FAIL(dependency_column_names_.assign(other.dependency_column_names_))) {
    }
  }
  return ret;
}

int AIColumnDef::deep_copy(const AIColumnDef &other, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  reset();
  column_id_ = other.column_id_;
  state_column_id_ = other.state_column_id_;
  request_column_id_ = other.request_column_id_;
  spec_column_id_ = other.spec_column_id_;
  function_type_ = other.function_type_;
  vector_dimension_ = other.vector_dimension_;
  max_retries_ = other.max_retries_;
  definition_version_ = other.definition_version_;
  if (OB_FAIL(ob_write_string(allocator, other.column_name_, column_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.state_column_name_, state_column_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.request_column_name_, request_column_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.spec_column_name_, spec_column_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.model_name_, model_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.expr_str_, expr_str_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.prompt_expr_, prompt_expr_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.normalized_expr_, normalized_expr_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.type_prompt_, type_prompt_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.config_, config_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.result_type_, result_type_))) {
  } else if (OB_FAIL(dependency_column_ids_.assign(other.dependency_column_ids_))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < other.dependency_column_names_.count(); ++i) {
    ObString name;
    if (OB_FAIL(ob_write_string(allocator, other.dependency_column_names_.at(i), name))) {
    } else if (OB_FAIL(dependency_column_names_.push_back(name))) {
    }
  }
  return ret;
}

int AIColumnMeta::deep_copy(const AIColumnMeta &other, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  defs_.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < other.defs_.count(); ++i) {
    AIColumnDef def;
    if (OB_FAIL(def.deep_copy(other.defs_.at(i), allocator))) {
    } else if (OB_FAIL(defs_.push_back(def))) {
    }
  }
  return ret;
}

const AIColumnDef *AIColumnMeta::get_def(const uint64_t column_id) const
{
  const AIColumnDef *def = NULL;
  for (int64_t i = 0; NULL == def && i < defs_.count(); ++i) {
    if (column_id == defs_.at(i).column_id_) {
      def = &defs_.at(i);
    }
  }
  return def;
}

AIColumnDef *AIColumnMeta::get_def(const uint64_t column_id)
{
  AIColumnDef *def = NULL;
  for (int64_t i = 0; NULL == def && i < defs_.count(); ++i) {
    if (column_id == defs_.at(i).column_id_) {
      def = &defs_.at(i);
    }
  }
  return def;
}

const AIColumnDef *AIColumnMeta::get_def_by_name(const ObString &column_name) const
{
  const AIColumnDef *def = NULL;
  for (int64_t i = 0; NULL == def && i < defs_.count(); ++i) {
    if (0 == defs_.at(i).column_name_.case_compare(column_name)) {
      def = &defs_.at(i);
    }
  }
  return def;
}

const AIColumnDef *AIColumnMeta::get_def_by_hidden_column(const uint64_t column_id) const
{
  const AIColumnDef *def = NULL;
  for (int64_t i = 0; NULL == def && i < defs_.count(); ++i) {
    if (defs_.at(i).is_hidden_column(column_id)) {
      def = &defs_.at(i);
    }
  }
  return def;
}

bool AIColumnMeta::is_dependency_column(const uint64_t column_id) const
{
  bool found = false;
  for (int64_t i = 0; !found && i < defs_.count(); ++i) {
    found = defs_.at(i).depends_on(column_id);
  }
  return found;
}

int AIColumnMeta::remove_def(const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  for (int64_t i = defs_.count() - 1; OB_SUCC(ret) && i >= 0; --i) {
    if (column_id == defs_.at(i).column_id_) {
      if (OB_FAIL(defs_.remove(i))) {
      }
    }
  }
  return ret;
}

int AIColumnSchemaUtil::decode(const ObString &data, AIColumnMeta &meta)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  meta.reset();
  if (data.empty()) {
    // no AI column
  } else if (OB_FAIL(meta.deserialize(data.ptr(), data.length(), pos))) {
    LOG_WARN("failed to deserialize ai column meta", K(ret), K(data.length()));
  }
  return ret;
}

int AIColumnSchemaUtil::encode(const AIColumnMeta &meta, ObIAllocator &allocator, ObString &data)
{
  int ret = OB_SUCCESS;
  data.reset();
  if (!meta.empty()) {
    const int64_t buf_len = meta.get_serialize_size();
    int64_t pos = 0;
    char *buf = static_cast<char *>(allocator.alloc(buf_len));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(meta.serialize(buf, buf_len, pos))) {
      LOG_WARN("failed to serialize ai column meta", K(ret));
    } else {
      data.assign_ptr(buf, static_cast<ObString::obstr_size_t>(pos));
    }
  }
  return ret;
}

int AIColumnSchemaUtil::get_meta(const ObTableSchema &table_schema, AIColumnMeta &meta)
{
  return decode(table_schema.get_ai_column_meta(), meta);
}

int AIColumnSchemaUtil::set_meta(const AIColumnMeta &meta, ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColumnMeta");
  ObString data;
  if (OB_FAIL(encode(meta, allocator, data))) {
  } else if (OB_FAIL(table_schema.set_ai_column_meta(data))) {
  }
  return ret;
}

int AIColumnSchemaUtil::fill_column_ids(const ObTableSchema &table_schema, AIColumnMeta &meta)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < meta.defs_.count(); ++i) {
    AIColumnDef &def = meta.defs_.at(i);
    const ObColumnSchemaV2 *col = NULL;
    const ObString *names[4] = {&def.column_name_, &def.state_column_name_,
                                &def.request_column_name_, &def.spec_column_name_};
    uint64_t *ids[4] = {&def.column_id_, &def.state_column_id_,
                        &def.request_column_id_, &def.spec_column_id_};
    // names are authoritative: column ids are only known for sure once the DDL service built
    // the final table schema
    for (int64_t j = 0; OB_SUCC(ret) && j < 4; ++j) {
      if (OB_ISNULL(col = table_schema.get_column_schema(*names[j]))) {
        ret = OB_ERR_BAD_FIELD_ERROR;
        LOG_WARN("ai column related column not found", K(ret), K(*names[j]));
      } else {
        *ids[j] = col->get_column_id();
      }
    }
    if (OB_SUCC(ret)) {
      def.dependency_column_ids_.reset();
      for (int64_t j = 0; OB_SUCC(ret) && j < def.dependency_column_names_.count(); ++j) {
        if (OB_ISNULL(col = table_schema.get_column_schema(def.dependency_column_names_.at(j)))) {
          ret = OB_ERR_BAD_FIELD_ERROR;
          LOG_WARN("ai column dependency not found", K(ret), K(def.dependency_column_names_.at(j)));
        } else if (OB_FAIL(def.dependency_column_ids_.push_back(col->get_column_id()))) {
        }
      }
    }
  }
  return ret;
}

const ObColumnSchemaV2 *AIColumnSchemaUtil::find_column(const ObTableSchema &table_schema,
                                                        const ObString &name)
{
  const ObColumnSchemaV2 *column = NULL;
  for (ObTableSchema::const_column_iterator iter = table_schema.column_begin();
       NULL == column && iter != table_schema.column_end(); ++iter) {
    if (NULL != *iter && 0 == (*iter)->get_column_name_str().case_compare(name)) {
      column = *iter;
    }
  }
  return column;
}

int AIColumnSchemaUtil::build_hidden_column_name(const char *prefix,
                                                 const ObString &ai_column_name,
                                                 const ObTableSchema &table_schema,
                                                 const ObTableSchema *added_columns,
                                                 ObIAllocator &allocator,
                                                 ObString &name)
{
  int ret = OB_SUCCESS;
  ObSqlString base;
  const int64_t prefix_len = STRLEN(prefix);
  // keep room for the prefix and a uniqueness suffix
  const int64_t max_name_len = OB_MAX_COLUMN_NAME_LENGTH - prefix_len - 8;
  if (ai_column_name.length() <= max_name_len) {
    ret = base.append_fmt("%s%.*s", prefix, ai_column_name.length(), ai_column_name.ptr());
  } else {
    const uint64_t hash = murmurhash(ai_column_name.ptr(), ai_column_name.length(), 0);
    ret = base.append_fmt("%s%.*s_%08lx", prefix, static_cast<int>(max_name_len - 9),
                          ai_column_name.ptr(), hash & 0xFFFFFFFF);
  }
  ObSqlString candidate;
  bool found = false;
  for (int64_t i = 0; OB_SUCC(ret) && !found && i < 1000; ++i) {
    candidate.reuse();
    if (0 == i) {
      ret = candidate.assign(base.string());
    } else {
      ret = candidate.append_fmt("%.*s_%ld", static_cast<int>(base.length()), base.ptr(), i);
    }
    if (OB_SUCC(ret) && NULL == table_schema.get_column_schema(candidate.string())
        && (NULL == added_columns || NULL == find_column(*added_columns, candidate.string()))) {
      found = true;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!found) {
    ret = OB_ERR_COLUMN_DUPLICATE;
    LOG_WARN("can not find a free hidden column name", K(ret), K(ai_column_name));
  } else if (OB_FAIL(ob_write_string(allocator, candidate.string(), name))) {
  }
  return ret;
}

int AIColumnSchemaUtil::build_type_prompt(const ObColumnSchemaV2 &column,
                                          ObIAllocator &allocator,
                                          ObString &prompt)
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  const ObObjType type = column.get_data_type();
  if (ob_is_json(type)) {
    ret = buf.assign("Return exactly one valid JSON value and nothing else. "
                     "Do not wrap it in markdown code fences or add any explanation.");
  } else if (ob_is_integer_type(type) || ObYearType == type || ob_is_bit_tc(type)) {
    if (ObYearType == type) {
      ret = buf.assign("Return exactly one four-digit year as a base-10 integer and nothing else.");
    } else if (ob_is_unsigned_type(type) || ob_is_bit_tc(type)) {
      ret = buf.assign("Return exactly one non-negative base-10 integer and nothing else. "
                       "Do not add units, words or punctuation.");
    } else {
      ret = buf.assign("Return exactly one base-10 integer and nothing else. "
                       "Do not add units, words or punctuation.");
    }
  } else if (ob_is_number_tc(type) || ob_is_decimal_int_tc(type)) {
    if (column.get_data_scale() > 0) {
      ret = buf.append_fmt("Return exactly one decimal number with at most %d digits after the "
                           "decimal point and nothing else.", column.get_data_scale());
    } else {
      ret = buf.assign("Return exactly one decimal number and nothing else.");
    }
  } else if (ob_is_float_tc(type) || ob_is_double_tc(type)) {
    ret = buf.assign("Return exactly one decimal number and nothing else.");
  } else if (ObDateType == type || ObMySQLDateType == type) {
    ret = buf.assign("Return exactly one date in YYYY-MM-DD format and nothing else.");
  } else if (ObDateTimeType == type || ObTimestampType == type || ObMySQLDateTimeType == type) {
    ret = buf.assign("Return exactly one date and time in YYYY-MM-DD HH:MM:SS format and nothing else.");
  } else if (ObTimeType == type) {
    ret = buf.assign("Return exactly one time in HH:MM:SS format and nothing else.");
  } else if (ob_is_enum_or_set_type(type)) {
    const ObIArray<ObString> &values = column.get_extended_type_info();
    if (ObEnumType == type) {
      ret = buf.assign("Return exactly one of the following values and nothing else: ");
    } else {
      ret = buf.assign("Return a comma-separated list chosen from the following values and nothing else: ");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < values.count(); ++i) {
      ret = buf.append_fmt("%s%.*s", 0 == i ? "" : ", ", values.at(i).length(), values.at(i).ptr());
    }
    if (OB_SUCC(ret)) {
      ret = buf.append(".");
    }
  } else if (ob_is_string_tc(type)) {
    ret = buf.append_fmt("Return only the answer text, at most %d characters, without explanations.",
                         column.get_data_length());
  } else if (ob_is_text_tc(type)) {
    ret = buf.assign("Return only the answer text without explanations.");
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, buf.string(), prompt))) {
  }
  return ret;
}

int AIColumnSchemaUtil::build_result_type_str(const ObColumnSchemaV2 &column,
                                              ObIAllocator &allocator,
                                              ObString &type_str)
{
  int ret = OB_SUCCESS;
  char buf[OB_MAX_SYS_PARAM_NAME_LENGTH] = {0};
  int64_t pos = 0;
  if (column.is_collection()) {
    const ObIArray<ObString> &infos = column.get_extended_type_info();
    if (infos.count() > 0) {
      ret = databuff_printf(buf, sizeof(buf), pos, "%.*s", infos.at(0).length(), infos.at(0).ptr());
    } else {
      ret = databuff_printf(buf, sizeof(buf), pos, "collection");
    }
  } else if (OB_FAIL(ob_sql_type_str(buf, sizeof(buf), pos, column.get_data_type(),
                                     column.get_data_length(), column.get_data_precision(),
                                     column.get_data_scale(), column.get_collation_type(),
                                     column.get_extended_type_info()))) {
    // fall back to the plain type name
    ret = OB_SUCCESS;
    pos = 0;
    ret = databuff_printf(buf, sizeof(buf), pos, "%s", ob_obj_type_str(column.get_data_type()));
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, ObString(pos, buf), type_str))) {
  }
  return ret;
}

const char *AIColumnSchemaUtil::get_state_name(const int64_t state)
{
  const char *name = "PENDING";
  if (AI_COLUMN_STATE_READY == state) {
    name = "READY";
  } else if (AI_COLUMN_STATE_FAILED == state) {
    name = "FAILED";
  }
  return name;
}

static int ai_column_join_ids(const ObIArray<uint64_t> &ids, ObSqlString &str)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < ids.count(); ++i) {
    ret = str.append_fmt("%s%lu", 0 == i ? "" : ",", ids.at(i));
  }
  return ret;
}

static int ai_column_join_names(const ObIArray<ObString> &names, ObSqlString &str)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    ret = str.append_fmt("%s%.*s", 0 == i ? "" : ",", names.at(i).length(), names.at(i).ptr());
  }
  return ret;
}

int AIColumnSqlService::insert_defs(ObISQLClient &sql_client,
                                    const uint64_t table_id,
                                    const AIColumnMeta &meta)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < meta.defs_.count(); ++i) {
    const AIColumnDef &def = meta.defs_.at(i);
    ObDMLSqlSplicer dml;
    ObSqlString sql;
    ObSqlString dep_ids;
    ObSqlString dep_names;
    int64_t affected_rows = 0;
    if (OB_UNLIKELY(!def.is_valid())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid ai column definition", K(ret), K(def));
    } else if (OB_FAIL(ai_column_join_ids(def.dependency_column_ids_, dep_ids))) {
    } else if (OB_FAIL(ai_column_join_names(def.dependency_column_names_, dep_names))) {
    } else if (OB_FAIL(dml.add_pk_column("table_id", table_id))
               || OB_FAIL(dml.add_pk_column("column_id", def.column_id_))
               || OB_FAIL(dml.add_column("column_name", ObHexEscapeSqlStr(def.column_name_)))
               || OB_FAIL(dml.add_column("state_column_id", def.state_column_id_))
               || OB_FAIL(dml.add_column("request_column_id", def.request_column_id_))
               || OB_FAIL(dml.add_column("result_spec_column_id", def.spec_column_id_))
               || OB_FAIL(dml.add_column("function_name", def.get_function_name()))
               || OB_FAIL(dml.add_column("model_name", ObHexEscapeSqlStr(def.model_name_)))
               || OB_FAIL(dml.add_column("expr_str", ObHexEscapeSqlStr(def.expr_str_)))
               || OB_FAIL(dml.add_column("normalized_expr", ObHexEscapeSqlStr(def.normalized_expr_)))
               || OB_FAIL(dml.add_column("prompt_expr", ObHexEscapeSqlStr(def.prompt_expr_)))
               || OB_FAIL(dml.add_column("type_prompt", ObHexEscapeSqlStr(def.type_prompt_)))
               || OB_FAIL(dml.add_column("config", ObHexEscapeSqlStr(def.config_)))
               || OB_FAIL(dml.add_column("dependency_column_ids", dep_ids.ptr()))
               || OB_FAIL(dml.add_column("dependency_column_names", ObHexEscapeSqlStr(dep_names.string())))
               || OB_FAIL(dml.add_column("result_type", ObHexEscapeSqlStr(def.result_type_)))
               || OB_FAIL(dml.add_column("vector_dimension", def.vector_dimension_))
               || OB_FAIL(dml.add_column("sync_mode", "async"))
               || OB_FAIL(dml.add_column("max_retries", def.max_retries_))
               || OB_FAIL(dml.add_column("definition_version", def.definition_version_))) {
      LOG_WARN("failed to build ai column row", K(ret));
    } else if (OB_FAIL(dml.splice_insert_sql(OB_ALL_AI_COLUMN_TNAME, sql))) {
    } else if (OB_FAIL(sql_client.write(sql.ptr(), affected_rows))) {
      LOG_WARN("failed to insert ai column", K(ret), K(sql));
    } else if (OB_UNLIKELY(1 != affected_rows)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected affected rows", K(ret), K(affected_rows));
    }
  }
  return ret;
}

int AIColumnSqlService::delete_defs(ObISQLClient &sql_client, const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu",
                             OB_ALL_AI_COLUMN_TNAME, table_id))) {
  } else if (OB_FAIL(sql_client.write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to delete ai columns", K(ret), K(sql));
  }
  return ret;
}

int AIColumnSqlService::replace_defs(ObISQLClient &sql_client,
                                     const uint64_t table_id,
                                     const AIColumnMeta &meta)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(delete_defs(sql_client, table_id))) {
  } else if (OB_FAIL(insert_defs(sql_client, table_id, meta))) {
  }
  return ret;
}

int AIColumnSqlService::delete_tasks(ObISQLClient &sql_client,
                                     const uint64_t table_id,
                                     const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu",
                             OB_ALL_AI_COLUMN_TASK_TNAME, table_id))) {
  } else if (OB_INVALID_ID != column_id
             && OB_FAIL(sql.append_fmt(" AND column_id = %lu", column_id))) {
  } else if (OB_FAIL(sql_client.write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to delete ai column tasks", K(ret), K(sql));
  }
  return ret;
}

static int ai_column_parse_ids(const ObString &str, ObIArray<uint64_t> &ids)
{
  int ret = OB_SUCCESS;
  ObString remain = str;
  while (OB_SUCC(ret) && !remain.empty()) {
    ObString item = remain.split_on(',');
    if (item.empty() && NULL == item.ptr()) {
      item = remain;
      remain.reset();
    }
    item = item.trim();
    if (!item.empty()) {
      char buf[32] = {0};
      if (item.length() >= sizeof(buf)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        MEMCPY(buf, item.ptr(), item.length());
        ret = ids.push_back(strtoull(buf, NULL, 10));
      }
    }
  }
  return ret;
}

static int ai_column_parse_names(const ObString &str, ObIArray<ObString> &names)
{
  int ret = OB_SUCCESS;
  ObString remain = str;
  while (OB_SUCC(ret) && !remain.empty()) {
    ObString item = remain.split_on(',');
    if (item.empty() && NULL == item.ptr()) {
      item = remain;
      remain.reset();
    }
    if (!item.empty()) {
      ret = names.push_back(item);
    }
  }
  return ret;
}

// __all_ai_column refers to the related columns by id, their names are taken from the table
// schema. A definition whose columns the schema lacks belongs to a later version of the table.
static int attach_meta(const AIColumnMeta &meta, ObTableSchema &schema)
{
  int ret = OB_SUCCESS;
  AIColumnMeta resolved;
  for (int64_t i = 0; OB_SUCC(ret) && i < meta.defs_.count(); ++i) {
    AIColumnDef def;
    bool complete = true;
    if (OB_FAIL(def.assign(meta.defs_.at(i)))) {
    } else {
      const uint64_t ids[4] = {def.column_id_, def.state_column_id_,
                               def.request_column_id_, def.spec_column_id_};
      ObString *names[4] = {&def.column_name_, &def.state_column_name_,
                            &def.request_column_name_, &def.spec_column_name_};
      for (int64_t j = 0; complete && j < 4; ++j) {
        const ObColumnSchemaV2 *col = schema.get_column_schema(ids[j]);
        if (OB_ISNULL(col)) {
          complete = false;
        } else {
          *names[j] = col->get_column_name_str();
        }
      }
      for (int64_t j = 0; complete && j < def.dependency_column_ids_.count(); ++j) {
        const ObColumnSchemaV2 *col = schema.get_column_schema(def.dependency_column_ids_.at(j));
        if (OB_ISNULL(col)) {
          complete = false;
        } else if (j < def.dependency_column_names_.count()) {
          def.dependency_column_names_.at(j) = col->get_column_name_str();
        }
      }
    }
    if (OB_SUCC(ret) && complete && OB_FAIL(resolved.defs_.push_back(def))) {
    }
  }
  if (OB_SUCC(ret) && !resolved.empty() && OB_FAIL(AIColumnSchemaUtil::set_meta(resolved, schema))) {
  }
  return ret;
}

int AIColumnSqlService::fetch_and_attach(ObISQLClient &sql_client,
                                         const ObIArray<uint64_t> &table_ids,
                                         ObIArray<ObTableSchema *> &table_schemas)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (table_ids.empty()) {
  } else if (OB_FAIL(sql.assign_fmt("SELECT table_id, column_id, column_name, state_column_id, "
                                    "request_column_id, result_spec_column_id, function_name, "
                                    "model_name, expr_str, normalized_expr, prompt_expr, type_prompt, "
                                    "config, dependency_column_ids, dependency_column_names, "
                                    "result_type, vector_dimension, max_retries, definition_version "
                                    "FROM %s WHERE table_id IN (", OB_ALL_AI_COLUMN_TNAME))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
      ret = sql.append_fmt("%s%lu", 0 == i ? "" : ",", table_ids.at(i));
    }
    if (OB_SUCC(ret)) {
      ret = sql.append(") ORDER BY table_id, column_id");
    }
  }
  if (OB_SUCC(ret) && !table_ids.empty()) {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      ObArenaAllocator allocator("AIColumnFetch");
      uint64_t cur_table_id = OB_INVALID_ID;
      AIColumnMeta meta;
      auto attach = [&]() -> int {
        int tmp_ret = OB_SUCCESS;
        for (int64_t i = 0; OB_SUCCESS == tmp_ret && i < table_schemas.count(); ++i) {
          ObTableSchema *schema = table_schemas.at(i);
          if (OB_NOT_NULL(schema) && schema->get_table_id() == cur_table_id) {
            tmp_ret = attach_meta(meta, *schema);
          }
        }
        return tmp_ret;
      };
      if (OB_FAIL(sql_client.read(res, sql.ptr()))) {
        if (OB_TABLE_NOT_EXIST == ret || OB_ERR_BAD_TABLE == ret) {
          // tolerate a data directory created before AI columns existed
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("failed to read ai columns", K(ret), K(sql));
        }
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          int64_t table_id = 0;
          int64_t column_id = 0;
          int64_t state_column_id = 0;
          int64_t request_column_id = 0;
          int64_t spec_column_id = 0;
          ObString function_name;
          ObString dep_ids;
          ObString dep_names;
          AIColumnDef row;
          AIColumnDef def;
          EXTRACT_INT_FIELD_MYSQL(*result, "table_id", table_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "column_id", column_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "state_column_id", state_column_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "request_column_id", request_column_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "result_spec_column_id", spec_column_id, int64_t);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "column_name", row.column_name_);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "function_name", function_name);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "model_name", row.model_name_);
          EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(*result, "expr_str", row.expr_str_);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "normalized_expr", row.normalized_expr_);
          EXTRACT_VARCHAR_FIELD_MYSQL(*result, "prompt_expr", row.prompt_expr_);
          EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(*result, "type_prompt", row.type_prompt_);
          EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(*result, "config", row.config_);
          EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(*result, "dependency_column_ids", dep_ids);
          EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(*result, "dependency_column_names", dep_names);
          EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(*result, "result_type", row.result_type_);
          EXTRACT_INT_FIELD_MYSQL(*result, "vector_dimension", row.vector_dimension_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "max_retries", row.max_retries_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "definition_version", row.definition_version_, int64_t);
          if (OB_SUCC(ret)) {
            row.column_id_ = column_id;
            row.state_column_id_ = state_column_id;
            row.request_column_id_ = request_column_id;
            row.spec_column_id_ = spec_column_id;
            row.function_type_ = 0 == function_name.case_compare("AI_EMBED")
                                 ? AI_COLUMN_FUNC_EMBED : AI_COLUMN_FUNC_COMPLETE;
            if (OB_FAIL(ai_column_parse_ids(dep_ids, row.dependency_column_ids_))) {
            } else if (OB_FAIL(ai_column_parse_names(dep_names, row.dependency_column_names_))) {
            } else if (OB_FAIL(def.deep_copy(row, allocator))) {
            } else if (cur_table_id != static_cast<uint64_t>(table_id)) {
              if (OB_INVALID_ID != cur_table_id && OB_FAIL(attach())) {
              } else {
                cur_table_id = table_id;
                meta.reset();
              }
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(meta.defs_.push_back(def))) {
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          if (OB_INVALID_ID != cur_table_id && OB_FAIL(attach())) {
          }
        }
      }
    }
  }
  return ret;
}

int AIColumnSqlService::attach_to_user_tables(ObISQLClient &sql_client,
                                              ObIArray<ObTableSchema *> &table_schemas)
{
  int ret = OB_SUCCESS;
  static const int64_t BATCH_SIZE = 256;
  ObSEArray<uint64_t, 16> table_ids;
  ObSEArray<ObTableSchema *, 16> tables;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schemas.count(); ++i) {
    ObTableSchema *table = table_schemas.at(i);
    if (OB_ISNULL(table) || !table->is_user_table() || is_inner_table(table->get_table_id())) {
      // only user tables can have AI columns
    } else if (OB_FAIL(table_ids.push_back(table->get_table_id()))) {
    } else if (OB_FAIL(tables.push_back(table))) {
    }
    if (OB_SUCC(ret) && (table_ids.count() >= BATCH_SIZE
                         || (i + 1 == table_schemas.count() && !table_ids.empty()))) {
      if (OB_FAIL(fetch_and_attach(sql_client, table_ids, tables))) {
        LOG_WARN("failed to attach ai columns", K(ret));
      } else {
        table_ids.reuse();
        tables.reuse();
      }
    }
  }
  return ret;
}

int AIColumnSqlService::on_create_tables(ObISQLClient &sql_client,
                                         ObIArray<ObTableSchema> &tables,
                                         const bool is_truncate_table)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
    ObTableSchema &table = tables.at(i);
    if (table.has_ai_column()) {
      ObArenaAllocator allocator("AIColumnDDL");
      AIColumnMeta decoded;
      AIColumnMeta meta;
      if (OB_FAIL(AIColumnSchemaUtil::get_meta(table, decoded))) {
      } else if (OB_FAIL(meta.deep_copy(decoded, allocator))) {
      } else if (OB_FAIL(AIColumnSchemaUtil::fill_column_ids(table, meta))) {
      } else if (OB_FAIL(AIColumnSchemaUtil::set_meta(meta, table))) {
      } else if (OB_FAIL(replace_defs(sql_client, table.get_table_id(), meta))) {
      } else if (is_truncate_table
                 && OB_FAIL(delete_tasks(sql_client, table.get_table_id(), OB_INVALID_ID))) {
      }
    }
  }
  return ret;
}

int AIColumnSqlService::on_drop_table(ObISQLClient &sql_client, const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  if (table.has_ai_column()) {
    if (OB_FAIL(delete_defs(sql_client, table.get_table_id()))) {
    } else if (OB_FAIL(delete_tasks(sql_client, table.get_table_id(), OB_INVALID_ID))) {
    }
  }
  return ret;
}

int AIColumnSqlService::on_alter_table(ObISQLClient &sql_client,
                                       const ObTableSchema &orig_table,
                                       const ObTableSchema &alter_table,
                                       ObTableSchema &new_table)
{
  int ret = OB_SUCCESS;
  if (alter_table.has_ai_column()) {
    ObArenaAllocator allocator("AIColumnDDL");
    AIColumnMeta orig_meta;
    AIColumnMeta delta;
    AIColumnMeta merged;
    if (OB_FAIL(AIColumnSchemaUtil::get_meta(orig_table, orig_meta))) {
    } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(alter_table, delta))) {
    } else if (OB_FAIL(merged.deep_copy(orig_meta, allocator))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < delta.defs_.count(); ++i) {
      const AIColumnDef &def = delta.defs_.at(i);
      bool replaced = false;
      for (int64_t j = 0; OB_SUCC(ret) && !replaced && j < merged.defs_.count(); ++j) {
        if (0 == merged.defs_.at(j).column_name_.case_compare(def.column_name_)) {
          replaced = true;
          ret = merged.defs_.at(j).deep_copy(def, allocator);
        }
      }
      if (OB_SUCC(ret) && !replaced) {
        AIColumnDef copied;
        if (OB_FAIL(copied.deep_copy(def, allocator))) {
        } else if (OB_FAIL(merged.defs_.push_back(copied))) {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(AIColumnSchemaUtil::fill_column_ids(new_table, merged))) {
      LOG_WARN("failed to fill ai column ids", K(ret), K(merged));
    } else if (OB_FAIL(AIColumnSchemaUtil::set_meta(merged, new_table))) {
    } else if (OB_FAIL(replace_defs(sql_client, new_table.get_table_id(), merged))) {
    }
    // tasks of a redefined column are obsolete, rows are regenerated after the DDL
    for (int64_t i = 0; OB_SUCC(ret) && i < delta.defs_.count(); ++i) {
      const AIColumnDef *def = merged.get_def_by_name(delta.defs_.at(i).column_name_);
      if (OB_NOT_NULL(def) && OB_FAIL(delete_tasks(sql_client, new_table.get_table_id(),
                                                     def->column_id_))) {
      }
    }
  }
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase
