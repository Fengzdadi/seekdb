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

#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "lib/string/ob_sql_string.h"
#include "share/schema/ob_ai_model_mgr.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "sql/resolver/ddl/ob_alter_table_stmt.h"
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_insert_stmt.h"
#include "sql/resolver/dml/ob_update_stmt.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/resolver/ob_schema_checker.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{

const ObColumnSchemaV2 *AIColumnScope::get_column(const ObString &name) const
{
  const ObColumnSchemaV2 *column = NULL;
  if (NULL != table_) {
    column = table_->get_column_schema(name);
  }
  if (NULL == column && NULL != added_) {
    column = AIColumnSchemaUtil::find_column(*added_, name);
  }
  return column;
}

bool AIColumnScope::is_ai_column(const ObString &name) const
{
  bool found = false;
  for (int64_t i = 0; !found && i < ai_names_.count(); ++i) {
    found = (0 == ai_names_.at(i).case_compare(name));
  }
  return found;
}

static int ai_column_get_definition_name(const ParseNode &column_definition, ObString &name)
{
  int ret = OB_SUCCESS;
  const ParseNode *ref = column_definition.num_child_ > 0 ? column_definition.children_[0] : NULL;
  if (OB_ISNULL(ref) || OB_UNLIKELY(T_COLUMN_REF != ref->type_ || ref->num_child_ < 3)
      || OB_ISNULL(ref->children_[2])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid column definition", K(ret));
  } else {
    name.assign_ptr(ref->children_[2]->str_value_,
                    static_cast<int32_t>(ref->children_[2]->str_len_));
  }
  return ret;
}

// the value of a constant string argument, is_const is false for any other expression
static void ai_column_get_const_string(const ObRawExpr *expr, ObString &str, bool &is_const)
{
  is_const = false;
  str.reset();
  if (NULL != expr && expr->is_const_raw_expr()) {
    const ObObj &value = static_cast<const ObConstRawExpr *>(expr)->get_value();
    if (value.is_string_type()) {
      str = value.get_string();
      is_const = true;
    } else if (value.is_null()) {
      is_const = true;
    }
  }
}

static void ai_column_get_const_int(const ObRawExpr *expr, int64_t &value, bool &is_const)
{
  is_const = false;
  value = 0;
  if (NULL != expr && expr->is_const_raw_expr()) {
    const ObObj &obj = static_cast<const ObConstRawExpr *>(expr)->get_value();
    if (ob_is_int_tc(obj.get_type())) {
      value = obj.get_int();
      is_const = true;
    } else if (ob_is_uint_tc(obj.get_type()) && obj.get_uint64() <= INT64_MAX) {
      value = static_cast<int64_t>(obj.get_uint64());
      is_const = true;
    }
  }
}

static int ai_column_print_expr(ObRawExpr &expr,
                                ObSchemaGetterGuard *schema_guard,
                                const ObTimeZoneInfo *tz_info,
                                ObIAllocator &allocator,
                                ObString &str)
{
  int ret = OB_SUCCESS;
  HEAP_VAR(char[OB_MAX_DEFAULT_VALUE_LENGTH], buf) {
    int64_t pos = 0;
    ObRawExprPrinter printer(buf, OB_MAX_DEFAULT_VALUE_LENGTH, &pos, schema_guard, tz_info);
    if (OB_FAIL(printer.do_print(&expr, T_NONE_SCOPE, true))) {
      LOG_WARN("failed to print expr", K(ret));
    } else if (OB_FAIL(ob_write_string(allocator, ObString(pos, buf), str))) {
    }
  }
  return ret;
}

static int ai_column_append_quoted(ObSqlString &sql, const ObString &str)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
    const char c = str.ptr()[i];
    if ('\'' == c) {
      ret = sql.append("''");
    } else if ('\\' == c) {
      ret = sql.append("\\\\");
    } else {
      ret = sql.append(ObString(1, &str.ptr()[i]));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
  }
  return ret;
}

static bool ai_column_has_name(const ObIArray<ObString> &names, const ObString &name)
{
  bool found = false;
  for (int64_t i = 0; !found && i < names.count(); ++i) {
    found = (0 == names.at(i).case_compare(name));
  }
  return found;
}

static bool ai_column_is_complete_result_type(const ObObjType type)
{
  return ob_is_json(type) || ob_is_integer_type(type) || ObYearType == type || ob_is_bit_tc(type)
      || ob_is_number_tc(type) || ob_is_decimal_int_tc(type) || ob_is_float_tc(type)
      || ob_is_double_tc(type) || ObDateType == type || ObMySQLDateType == type
      || ObDateTimeType == type || ObTimestampType == type || ObMySQLDateTimeType == type
      || ObTimeType == type || ob_is_enum_or_set_type(type) || ob_is_string_tc(type)
      || ob_is_text_tc(type);
}

const ParseNode *AIColumnResolver::get_ai_column_node(const ParseNode *column_definition)
{
  const ParseNode *ai_node = NULL;
  if (NULL != column_definition && T_COLUMN_DEFINITION == column_definition->type_
      && column_definition->num_child_ >= 3 && NULL != column_definition->children_[2]) {
    const ParseNode *attrs = column_definition->children_[2];
    for (int64_t i = 0; NULL == ai_node && i < attrs->num_child_; ++i) {
      if (NULL != attrs->children_[i] && T_AI_COLUMN == attrs->children_[i]->type_) {
        ai_node = attrs->children_[i];
      }
    }
  }
  return ai_node;
}

bool AIColumnResolver::has_ai_column(const ParseNode *table_element_list)
{
  bool found = false;
  for (int64_t i = 0; !found && NULL != table_element_list && i < table_element_list->num_child_;
       ++i) {
    found = (NULL != get_ai_column_node(table_element_list->children_[i]));
  }
  return found;
}

int AIColumnResolver::build_hidden_column(const ObString &name,
                                          const ObObjType type,
                                          ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  column.set_data_type(type);
  column.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[type]);
  column.set_nullable(true);
  column.set_is_hidden(true);
  column.set_charset_type(CHARSET_BINARY);
  column.set_collation_type(CS_TYPE_BINARY);
  if (OB_FAIL(column.set_column_name(name))) {
    LOG_WARN("failed to set column name", K(ret), K(name));
  }
  return ret;
}

int AIColumnResolver::get_vector_dimension(const ObColumnSchemaV2 &column, int64_t &dim)
{
  int ret = OB_SUCCESS;
  // a dense vector column is described as VECTOR(dim[, ...]), any other collection has no dim
  static const char VECTOR_PREFIX[] = "VECTOR(";
  const int64_t prefix_len = sizeof(VECTOR_PREFIX) - 1;
  const ObIArray<ObString> &infos = column.get_extended_type_info();
  dim = 0;
  if (column.is_collection() && 1 == infos.count() && infos.at(0).length() > prefix_len
      && 0 == STRNCASECMP(infos.at(0).ptr(), VECTOR_PREFIX, prefix_len)) {
    const ObString &info = infos.at(0);
    for (int64_t i = prefix_len; i < info.length() && info.ptr()[i] >= '0' && info.ptr()[i] <= '9'; ++i) {
      dim = dim * 10 + (info.ptr()[i] - '0');
    }
  }
  return ret;
}

int AIColumnResolver::resolve_options(const ParseNode *options, AIColumnDef &def)
{
  int ret = OB_SUCCESS;
  bool has_sync_mode = false;
  bool has_max_retries = false;
  def.max_retries_ = AIColumnSchemaUtil::DEFAULT_MAX_RETRIES;
  for (int64_t i = 0; OB_SUCC(ret) && NULL != options && i < options->num_child_; ++i) {
    const ParseNode *option = options->children_[i];
    if (OB_ISNULL(option) || OB_UNLIKELY(T_AI_COLUMN_OPTION != option->type_
                                         || 2 != option->num_child_)
        || OB_ISNULL(option->children_[0]) || OB_ISNULL(option->children_[1])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid ai column option", K(ret));
    } else {
      const ObString name(static_cast<int32_t>(option->children_[0]->str_len_),
                          option->children_[0]->str_value_);
      const ParseNode *value = option->children_[1];
      const ObString value_str(static_cast<int32_t>(value->str_len_), value->str_value_);
      if (0 == name.case_compare("sync_mode")) {
        if (has_sync_mode) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN option sync_mode specified twice");
        } else if ((T_VARCHAR != value->type_ && T_CHAR != value->type_ && T_IDENT != value->type_)
                   || 0 != value_str.case_compare("async")) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN sync_mode other than 'async'");
        } else {
          has_sync_mode = true;
        }
      } else if (0 == name.case_compare("max_retries")) {
        int64_t max_retries = -1;
        if (has_max_retries) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN option max_retries specified twice");
        } else if (T_INT == value->type_ || T_UINT64 == value->type_) {
          max_retries = value->value_;
        } else if ((T_VARCHAR == value->type_ || T_CHAR == value->type_) && !value_str.empty()) {
          max_retries = 0;
          for (int64_t j = 0; max_retries >= 0 && j < value_str.length(); ++j) {
            const char c = value_str.ptr()[j];
            if (c < '0' || c > '9' || max_retries > AIColumnSchemaUtil::MAX_MAX_RETRIES) {
              max_retries = -1;
            } else {
              max_retries = max_retries * 10 + (c - '0');
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else if (max_retries < 0 || max_retries > AIColumnSchemaUtil::MAX_MAX_RETRIES) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI COLUMN option max_retries");
        } else {
          has_max_retries = true;
          def.max_retries_ = max_retries;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN option other than sync_mode and max_retries");
      }
    }
  }
  return ret;
}

int AIColumnResolver::check_result_type(const ObColumnSchemaV2 &column, AIColumnDef &def)
{
  int ret = OB_SUCCESS;
  const ObObjType type = column.get_data_type();
  if (def.is_embed()) {
    int64_t column_dim = 0;
    if (column.is_collection()) {
      if (OB_FAIL(get_vector_dimension(column, column_dim))) {
      } else if (column_dim <= 0) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED result column of this type");
      } else if (def.vector_dimension_ > 0 && def.vector_dimension_ != column_dim) {
        ret = OB_ERR_INVALID_VECTOR_DIM;
        LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(column_dim),
                       static_cast<uint32_t>(def.vector_dimension_));
      } else {
        def.vector_dimension_ = column_dim;
      }
    } else if (!ob_is_string_tc(type) && !ob_is_text_tc(type) && !ob_is_json(type)) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_EMBED result column of this type");
    }
  } else if (!ai_column_is_complete_result_type(type)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI_COMPLETE result column of this type");
  }
  return ret;
}

int AIColumnResolver::resolve_definition(ObResolverParams &params,
                                         const ParseNode &ai_node,
                                         const ObColumnSchemaV2 &column,
                                         const AIColumnScope &scope,
                                         ObIAllocator &allocator,
                                         AIColumnDef &def)
{
  int ret = OB_SUCCESS;
  const ParseNode *expr_node = ai_node.num_child_ > 0 ? ai_node.children_[0] : NULL;
  const ParseNode *options = ai_node.num_child_ > 1 ? ai_node.children_[1] : NULL;
  const ObString &column_name = column.get_column_name_str();
  ObSchemaGetterGuard *schema_guard = NULL;
  ObRawExpr *expr = NULL;
  ObSEArray<ObQualifiedName, 4> columns;
  def.reset();
  if (OB_UNLIKELY(T_AI_COLUMN != ai_node.type_) || OB_ISNULL(expr_node)
      || OB_ISNULL(params.expr_factory_) || OB_ISNULL(params.session_info_)
      || OB_ISNULL(params.schema_checker_)
      || OB_ISNULL(schema_guard = params.schema_checker_->get_schema_guard())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid argument", K(ret), K(ai_node.type_));
  } else if (!column.is_nullable() || column.get_rowkey_position() > 0
             || column.is_autoincrement()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "NOT NULL, primary key or auto-increment AI COLUMN");
  } else if (OB_FAIL(resolve_options(options, def))) {
  } else if (OB_FAIL(ObRawExprUtils::build_generated_column_expr(*params.expr_factory_,
                                                                 *params.session_info_,
                                                                 *expr_node,
                                                                 expr,
                                                                 columns,
                                                                 NULL,
                                                                 NULL,
                                                                 params.schema_checker_,
                                                                 ObResolverUtils::DISABLE_CHECK,
                                                                 false))) {
    LOG_WARN("failed to build ai column expr", K(ret));
  } else if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (T_FUN_SYS_AI_COMPLETE == expr->get_expr_type()) {
    def.function_type_ = AI_COLUMN_FUNC_COMPLETE;
  } else if (T_FUN_SYS_AI_EMBED == expr->get_expr_type()) {
    def.function_type_ = AI_COLUMN_FUNC_EMBED;
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN expression other than AI_COMPLETE or AI_EMBED");
  }

  if (OB_SUCC(ret) && (expr->get_param_count() < 2 || expr->get_param_count() > 3)) {
    const ObString func_name(def.get_function_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name.length(), func_name.ptr());
  }

  // model: a constant naming an existing model of the matching type
  if (OB_SUCC(ret)) {
    ObString model_name;
    bool is_const = false;
    const ObAiModelSchema *model_schema = NULL;
    ai_column_get_const_string(expr->get_param_expr(0), model_name, is_const);
    if (!is_const) {
      ret = OB_AI_FUNC_PARAM_INVALID;
      LOG_USER_ERROR(OB_AI_FUNC_PARAM_INVALID, static_cast<int>(STRLEN("model")), "model");
    } else if (model_name.empty()) {
      ret = OB_AI_FUNC_PARAM_EMPTY;
      LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, static_cast<int>(STRLEN("model")), "model");
    } else if (OB_FAIL(schema_guard->get_ai_model_schema(model_name, model_schema))
               || OB_ISNULL(model_schema)) {
      ret = OB_AI_FUNC_MODEL_NOT_FOUND;
      LOG_USER_ERROR(OB_AI_FUNC_MODEL_NOT_FOUND, model_name.length(), model_name.ptr());
    } else if ((def.is_complete() && EndpointType::COMPLETION != model_schema->get_type())
               || (def.is_embed() && EndpointType::DENSE_EMBEDDING != model_schema->get_type())) {
      ret = OB_AI_FUNC_PARAM_INVALID;
      LOG_USER_ERROR(OB_AI_FUNC_PARAM_INVALID, static_cast<int>(STRLEN("model")), "model");
    } else if (OB_FAIL(ob_write_string(allocator, model_name, def.model_name_))) {
    }
  }

  // prompt (AI_COMPLETE) or input (AI_EMBED), kept as SQL text evaluated against the row
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(expr->get_param_expr(1))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ai_column_print_expr(*expr->get_param_expr(1), schema_guard,
                                            params.session_info_->get_timezone_info(),
                                            allocator, def.prompt_expr_))) {
    }
  }

  // dependencies: ordinary columns of the same table
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const ObQualifiedName &q_name = columns.at(i);
    const ObString &dep_name = q_name.col_name_;
    const ObColumnSchemaV2 *dep = NULL;
    if (q_name.is_star_ || !q_name.database_name_.empty()
        || (!q_name.tbl_name_.empty() && 0 != q_name.tbl_name_.case_compare(scope.table_name_))) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN referring to another table");
    } else if (OB_ISNULL(dep = scope.get_column(dep_name)) || dep->is_hidden()) {
      ret = OB_ERR_BAD_FIELD_ERROR;
      LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, dep_name.length(), dep_name.ptr(),
                     scope.table_name_.length(), scope.table_name_.ptr());
    } else if (0 == dep_name.case_compare(column_name) || scope.is_ai_column(dep_name)) {
      // AI columns can not be chained
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("ai column depends on an ai column", K(ret), K(column_name), K(dep_name));
    } else if (dep->is_generated_column()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN depending on a generated column");
    } else if (ai_column_has_name(def.dependency_column_names_, dep->get_column_name_str())) {
    } else {
      ObString dep_copy;
      if (OB_FAIL(ob_write_string(allocator, dep->get_column_name_str(), dep_copy))) {
      } else if (OB_FAIL(def.dependency_column_names_.push_back(dep_copy))) {
      } else if (OB_FAIL(def.dependency_column_ids_.push_back(dep->get_column_id()))) {
      }
    }
  }

  // config (AI_COMPLETE) or output dimension (AI_EMBED)
  if (OB_SUCC(ret) && 3 == expr->get_param_count()) {
    const ObRawExpr *third = expr->get_param_expr(2);
    bool is_const = false;
    if (def.is_complete()) {
      ObString config;
      ai_column_get_const_string(third, config, is_const);
      if (!is_const || config.empty()) {
        ret = OB_AI_FUNC_PARAM_INVALID;
        LOG_USER_ERROR(OB_AI_FUNC_PARAM_INVALID, static_cast<int>(STRLEN("config")), "config");
      } else if (OB_FAIL(ob_write_string(allocator, config, def.config_))) {
      }
    } else {
      int64_t dim = 0;
      ObSqlString config;
      ai_column_get_const_int(third, dim, is_const);
      if (!is_const || dim <= 0) {
        ret = OB_AI_FUNC_PARAM_INVALID;
        LOG_USER_ERROR(OB_AI_FUNC_PARAM_INVALID, static_cast<int>(STRLEN("dimension")),
                       "dimension");
      } else if (OB_FAIL(config.append_fmt("{\"dimensions\": %ld}", dim))) {
      } else if (OB_FAIL(ob_write_string(allocator, config.string(), def.config_))) {
      } else {
        def.vector_dimension_ = dim;
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(check_result_type(column, def))) {
  } else if (def.is_complete()
             && OB_FAIL(AIColumnSchemaUtil::build_type_prompt(column, allocator, def.type_prompt_))) {
  } else if (OB_FAIL(AIColumnSchemaUtil::build_result_type_str(column, allocator, def.result_type_))) {
  } else if (OB_FAIL(ob_write_string(allocator, column_name, def.column_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator,
                                     ObString(static_cast<int32_t>(expr_node->str_len_),
                                              expr_node->str_value_),
                                     def.expr_str_))) {
  } else if (def.expr_str_.empty()
             && OB_FAIL(ai_column_print_expr(*expr, schema_guard,
                                             params.session_info_->get_timezone_info(),
                                             allocator, def.expr_str_))) {
  } else if (OB_FAIL(build_normalized_expr(def, allocator, def.normalized_expr_))) {
  }
  return ret;
}

int AIColumnResolver::build_normalized_expr(const AIColumnDef &def,
                                            ObIAllocator &allocator,
                                            ObString &expr)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("%s(MODEL => ", def.get_function_name()))) {
  } else if (OB_FAIL(ai_column_append_quoted(sql, def.model_name_))) {
  } else if (def.is_complete()) {
    if (!def.type_prompt_.empty()
        && (OB_FAIL(sql.append(", SYSTEM => "))
            || OB_FAIL(ai_column_append_quoted(sql, def.type_prompt_)))) {
    } else if (OB_FAIL(sql.append_fmt(", PROMPT => %.*s", def.prompt_expr_.length(),
                                      def.prompt_expr_.ptr()))) {
    } else if (!def.config_.empty()
               && (OB_FAIL(sql.append(", CONFIG => "))
                   || OB_FAIL(ai_column_append_quoted(sql, def.config_)))) {
    }
  } else if (OB_FAIL(sql.append_fmt(", INPUT => %.*s", def.prompt_expr_.length(),
                                    def.prompt_expr_.ptr()))) {
  } else if (!def.config_.empty()
             && OB_FAIL(sql.append_fmt(", DIMENSIONS => %ld", def.vector_dimension_))) {
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(")"))) {
  } else if (OB_FAIL(ob_write_string(allocator, sql.string(), expr))) {
  }
  return ret;
}

int AIColumnResolver::resolve_create_table(ObResolverParams &params,
                                           const ParseNode &table_element_list,
                                           const ObString &table_name,
                                           const bool has_primary_key,
                                           ObTableSchema &table_schema,
                                           uint64_t &cur_column_id)
{
  int ret = OB_SUCCESS;
  AIColumnScope scope;
  ObSEArray<const ParseNode *, 4> ai_nodes;
  scope.table_name_ = table_name;
  scope.table_ = &table_schema;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_element_list.num_child_; ++i) {
    const ParseNode *element = table_element_list.children_[i];
    const ParseNode *ai_node = get_ai_column_node(element);
    ObString name;
    if (NULL == ai_node) {
    } else if (OB_FAIL(ai_column_get_definition_name(*element, name))) {
    } else if (OB_FAIL(ai_nodes.push_back(ai_node))) {
    } else if (OB_FAIL(scope.ai_names_.push_back(name))) {
    }
  }
  if (OB_SUCC(ret) && !ai_nodes.empty()) {
    AIColumnMeta meta;
    if (OB_ISNULL(params.allocator_)) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_nodes.count(); ++i) {
      const ObColumnSchemaV2 *column = table_schema.get_column_schema(scope.ai_names_.at(i));
      AIColumnDef def;
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column not found", K(ret), K(scope.ai_names_.at(i)));
      } else if (OB_FAIL(resolve_definition(params, *ai_nodes.at(i), *column, scope,
                                            *params.allocator_, def))) {
      } else if (FALSE_IT(def.column_id_ = column->get_column_id())) {
      } else if (OB_FAIL(meta.defs_.push_back(def))) {
      }
    }
    if (OB_SUCC(ret) && !has_primary_key) {
      // tasks address rows by their primary key
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN on a table without a primary key");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < meta.defs_.count(); ++i) {
      AIColumnDef &def = meta.defs_.at(i);
      const char *prefixes[3] = {AIColumnSchemaUtil::STATE_COLUMN_PREFIX,
                                 AIColumnSchemaUtil::REQUEST_COLUMN_PREFIX,
                                 AIColumnSchemaUtil::SPEC_COLUMN_PREFIX};
      const ObObjType types[3] = {ObTinyIntType, ObUInt64Type, ObIntType};
      ObString *names[3] = {&def.state_column_name_, &def.request_column_name_,
                            &def.spec_column_name_};
      uint64_t *ids[3] = {&def.state_column_id_, &def.request_column_id_, &def.spec_column_id_};
      for (int64_t j = 0; OB_SUCC(ret) && j < 3; ++j) {
        ObColumnSchemaV2 hidden;
        if (OB_FAIL(AIColumnSchemaUtil::build_hidden_column_name(prefixes[j], def.column_name_,
                                                                 table_schema, NULL,
                                                                 *params.allocator_, *names[j]))) {
        } else if (OB_FAIL(build_hidden_column(*names[j], types[j], hidden))) {
        } else {
          hidden.set_column_id(++cur_column_id);
          *ids[j] = cur_column_id;
          if (OB_FAIL(table_schema.add_column(hidden))) {
            LOG_WARN("failed to add ai hidden column", K(ret), K(hidden));
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (cur_column_id > table_schema.get_max_used_column_id()) {
        table_schema.set_max_used_column_id(cur_column_id);
      }
      if (OB_FAIL(AIColumnSchemaUtil::set_meta(meta, table_schema))) {
        LOG_WARN("failed to set ai column meta", K(ret));
      }
    }
  }
  return ret;
}

int AIColumnResolver::resolve_alter_column(ObResolverParams &params,
                                           const ParseNode &ai_node,
                                           const ObTableSchema &orig_table,
                                           const ObColumnSchemaV2 &column,
                                           const bool is_modify,
                                           ObAlterTableStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObTableSchema &alter_schema = stmt.get_alter_table_schema();
  const ObString &column_name = column.get_column_name_str();
  AIColumnMeta orig_meta;
  AIColumnMeta delta;
  AIColumnScope scope;
  AIColumnDef def;
  const AIColumnDef *orig_def = NULL;
  if (OB_ISNULL(params.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(orig_table, orig_meta))) {
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(alter_schema, delta))) {
  } else if (NULL != delta.get_def_by_name(column_name)) {
    ret = OB_ERR_COLUMN_DUPLICATE;
    LOG_USER_ERROR(OB_ERR_COLUMN_DUPLICATE, column_name.length(), column_name.ptr());
  } else {
    scope.table_name_ = orig_table.get_table_name_str();
    scope.table_ = &orig_table;
    scope.added_ = &alter_schema;
    for (int64_t i = 0; OB_SUCC(ret) && i < orig_meta.defs_.count(); ++i) {
      ret = scope.ai_names_.push_back(orig_meta.defs_.at(i).column_name_);
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < delta.defs_.count(); ++i) {
      ret = scope.ai_names_.push_back(delta.defs_.at(i).column_name_);
    }
    if (OB_SUCC(ret)) {
      ret = scope.ai_names_.push_back(column_name);
    }
    orig_def = is_modify ? orig_meta.get_def_by_name(column_name) : NULL;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(resolve_definition(params, ai_node, column, scope, *params.allocator_, def))) {
  } else if (orig_table.is_table_without_pk()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "AI COLUMN on a table without a primary key");
  } else if (NULL != orig_def) {
    // redefinition keeps the hidden columns, the new version invalidates older requests
    def.definition_version_ = orig_def->definition_version_ + 1;
    if (OB_FAIL(ob_write_string(*params.allocator_, orig_def->state_column_name_,
                                def.state_column_name_))) {
    } else if (OB_FAIL(ob_write_string(*params.allocator_, orig_def->request_column_name_,
                                       def.request_column_name_))) {
    } else if (OB_FAIL(ob_write_string(*params.allocator_, orig_def->spec_column_name_,
                                       def.spec_column_name_))) {
    }
  } else {
    const char *prefixes[3] = {AIColumnSchemaUtil::STATE_COLUMN_PREFIX,
                               AIColumnSchemaUtil::REQUEST_COLUMN_PREFIX,
                               AIColumnSchemaUtil::SPEC_COLUMN_PREFIX};
    const ObObjType types[3] = {ObTinyIntType, ObUInt64Type, ObIntType};
    ObString *names[3] = {&def.state_column_name_, &def.request_column_name_,
                          &def.spec_column_name_};
    def.definition_version_ = 1;
    for (int64_t j = 0; OB_SUCC(ret) && j < 3; ++j) {
      AlterColumnSchema hidden;
      if (OB_FAIL(AIColumnSchemaUtil::build_hidden_column_name(prefixes[j], column_name, orig_table,
                                                               &alter_schema, *params.allocator_,
                                                               *names[j]))) {
      } else if (OB_FAIL(build_hidden_column(*names[j], types[j], hidden))) {
      } else {
        hidden.alter_type_ = OB_DDL_ADD_COLUMN;
        if (OB_FAIL(stmt.add_column(hidden))) {
          LOG_WARN("failed to add ai hidden column", K(ret), K(hidden));
        }
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(delta.defs_.push_back(def))) {
  } else if (OB_FAIL(AIColumnSchemaUtil::set_meta(delta, alter_schema))) {
  } else if (OB_FAIL(stmt.add_ai_refresh_column(def.column_name_))) {
  }
  return ret;
}

int AIColumnResolver::resolve_retry(ObResolverParams &params,
                                    const ParseNode &node,
                                    const int64_t action_count,
                                    const ObTableSchema &table,
                                    ObAlterTableStmt &stmt)
{
  int ret = OB_SUCCESS;
  const ParseNode *name_node = node.num_child_ > 0 ? node.children_[0] : NULL;
  const ParseNode *where_node = node.num_child_ > 1 ? node.children_[1] : NULL;
  AIColumnMeta meta;
  if (OB_UNLIKELY(T_ALTER_TABLE_RETRY_AI_COLUMN != node.type_) || OB_ISNULL(name_node)
      || OB_ISNULL(params.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid retry ai column node", K(ret), K(node.type_));
  } else if (action_count > 1) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "combining AI COLUMN RETRY with another ALTER action");
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(table, meta))) {
  } else {
    const ObString column_name(static_cast<int32_t>(name_node->str_len_), name_node->str_value_);
    const AIColumnDef *def = meta.get_def_by_name(column_name);
    ObString name_copy;
    ObString where_copy;
    if (NULL == def) {
      if (NULL == table.get_column_schema(column_name)) {
        ret = OB_ERR_BAD_FIELD_ERROR;
        LOG_USER_ERROR(OB_ERR_BAD_FIELD_ERROR, column_name.length(), column_name.ptr(),
                       table.get_table_name_str().length(), table.get_table_name_str().ptr());
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
      }
    } else if (NULL != where_node && OB_UNLIKELY(where_node->str_len_ <= 0)) {
      ret = OB_ERR_PARSER_SYNTAX;
      LOG_WARN("empty retry condition", K(ret));
    } else if (OB_FAIL(ob_write_string(*params.allocator_, def->column_name_, name_copy))) {
    } else if (NULL != where_node
               && OB_FAIL(ob_write_string(*params.allocator_,
                                          ObString(static_cast<int32_t>(where_node->str_len_),
                                                   where_node->str_value_),
                                          where_copy))) {
    } else {
      stmt.set_ai_column_retry(name_copy, where_copy);
    }
  }
  return ret;
}

int AIColumnResolver::check_alter_column_allowed(const ObTableSchema &orig_table,
                                                 const ObString &column_name,
                                                 const AlterKind kind,
                                                 const bool has_ai_clause)
{
  int ret = OB_SUCCESS;
  AIColumnMeta meta;
  if (has_ai_clause && ALTER_MODIFY != kind) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "renaming a column into an AI column");
  } else if (!orig_table.has_ai_column()) {
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(orig_table, meta))) {
  } else {
    const AIColumnDef *def = meta.get_def_by_name(column_name);
    bool is_hidden = false;
    bool is_dependency = false;
    for (int64_t i = 0; i < meta.defs_.count(); ++i) {
      const AIColumnDef &cur = meta.defs_.at(i);
      is_hidden = is_hidden || 0 == cur.state_column_name_.case_compare(column_name)
                  || 0 == cur.request_column_name_.case_compare(column_name)
                  || 0 == cur.spec_column_name_.case_compare(column_name);
      is_dependency = is_dependency || ai_column_has_name(cur.dependency_column_names_, column_name);
    }
    if (is_hidden) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "altering a hidden column of an AI column");
    } else if (NULL != def) {
      if (ALTER_DROP == kind) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "dropping an AI column");
      } else if (ALTER_RENAME == kind) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "renaming an AI column");
      } else if (!has_ai_clause) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "modifying an AI column without an AI COLUMN clause");
      }
    } else if (is_dependency) {
      if (ALTER_DROP == kind) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "dropping a column used by an AI column");
      } else if (ALTER_RENAME == kind) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "renaming a column used by an AI column");
      }
    }
  }
  return ret;
}


static bool ai_column_is_rowkey(const ObTableSchema &table_schema, const uint64_t column_id)
{
  const ObColumnSchemaV2 *column = table_schema.get_column_schema(column_id);
  return NULL != column && column->is_rowkey_column();
}

// CASE WHEN unchanged THEN column ELSE new_value END, converted to the column type
static int ai_column_build_reset_expr(ObResolverParams &params,
                                      ObRawExpr *unchanged,
                                      ObColumnRefRawExpr *column,
                                      ObRawExpr *new_value,
                                      ObRawExpr *&expr)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObRawExprUtils::build_case_when_expr(*params.expr_factory_, unchanged, column,
                                                   new_value, expr))) {
    LOG_WARN("failed to build case when expr", K(ret));
  } else if (OB_FAIL(expr->formalize(params.session_info_))) {
    LOG_WARN("failed to formalize expr", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_column_conv_expr(*params.expr_factory_,
                                                            *params.allocator_,
                                                            *column, expr,
                                                            params.session_info_))) {
    LOG_WARN("failed to build column conv expr", K(ret));
  }
  return ret;
}

int AIColumnResolver::resolve_dml_assignments(ObDMLResolver &resolver,
                                              const TableItem &table_item,
                                              const ObTableSchema &table_schema,
                                              ObIArray<ObAssignment> &assignments)
{
  int ret = OB_SUCCESS;
  ObResolverParams &params = resolver.params_;
  AIColumnMeta meta;
  if (!table_schema.has_ai_column()) {
  } else if (OB_ISNULL(params.session_info_) || OB_ISNULL(params.expr_factory_)
             || OB_ISNULL(params.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid resolver params", K(ret));
  } else if (params.session_info_->is_inner()) {
    // the AI column worker and RETRY write AI columns directly
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(table_schema, meta))) {
    LOG_WARN("failed to get ai column meta", K(ret));
  } else {
    bool touch_source = false;
    const int64_t user_assign_cnt = assignments.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < user_assign_cnt; ++i) {
      const ObColumnRefRawExpr *column = assignments.at(i).column_expr_;
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (assignments.at(i).is_implicit_) {
      } else if (meta.is_ai_column(column->get_column_id())
                 || meta.is_hidden_column(column->get_column_id())) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "assigning a value to an AI column");
      } else if (meta.is_dependency_column(column->get_column_id())
                 || ai_column_is_rowkey(table_schema, column->get_column_id())) {
        touch_source = true;
      }
    }
    if (OB_FAIL(ret) || !touch_source) {
    } else if (!table_item.is_basic_table()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "updating source columns of AI columns through a view");
    } else {
      ObSysFunRawExpr *request_expr = NULL;
      for (int64_t d = 0; OB_SUCC(ret) && d < meta.count(); ++d) {
        const AIColumnDef &def = meta.defs_.at(d);
        ObSEArray<ObRawExpr *, 4> unchanged_conds;
        ObRawExpr *unchanged = NULL;
        for (int64_t i = 0; OB_SUCC(ret) && i < user_assign_cnt; ++i) {
          ObAssignment &assign = assignments.at(i);
          const uint64_t column_id = assign.column_expr_->get_column_id();
          ObRawExpr *cond = NULL;
          if (assign.is_implicit_
              || (!def.depends_on(column_id) && !ai_column_is_rowkey(table_schema, column_id))) {
          } else if (OB_FAIL(ObRawExprUtils::build_common_binary_op_expr(*params.expr_factory_,
                                                                         T_OP_NSEQ,
                                                                         assign.column_expr_,
                                                                         assign.expr_,
                                                                         cond))) {
            LOG_WARN("failed to build null safe equal expr", K(ret));
          } else if (OB_FAIL(unchanged_conds.push_back(cond))) {
          }
        }
        if (OB_FAIL(ret) || unchanged_conds.empty()) {
          continue;
        } else if (1 == unchanged_conds.count()) {
          unchanged = unchanged_conds.at(0);
        } else if (OB_FAIL(ObRawExprUtils::build_and_expr(*params.expr_factory_,
                                                          unchanged_conds, unchanged))) {
          LOG_WARN("failed to build and expr", K(ret));
        }
        if (OB_SUCC(ret) && NULL == request_expr) {
          // one new request id per row, it marks the change for the DML operator
          if (OB_FAIL(params.expr_factory_->create_raw_expr(T_FUN_SYS_UUID_SHORT, request_expr))) {
            LOG_WARN("failed to create uuid_short expr", K(ret));
          } else if (OB_ISNULL(request_expr)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            request_expr->set_func_name(ObString::make_string("uuid_short"));
            if (OB_FAIL(request_expr->formalize(params.session_info_))) {
              LOG_WARN("failed to formalize uuid_short expr", K(ret));
            }
          }
        }
        const uint64_t reset_ids[3] = {def.column_id_, def.state_column_id_, def.request_column_id_};
        for (int64_t k = 0; OB_SUCC(ret) && k < 3; ++k) {
          const ObColumnSchemaV2 *column_schema = table_schema.get_column_schema(reset_ids[k]);
          ObSEArray<ObColumnRefRawExpr *, 1> column_exprs;
          ObRawExpr *new_value = NULL;
          ObRawExpr *expr = NULL;
          ObAssignment assign;
          if (OB_ISNULL(column_schema)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ai column schema not found", K(ret), K(reset_ids[k]));
          } else if (OB_FAIL(resolver.add_column_to_stmt(table_item, *column_schema, column_exprs))) {
            LOG_WARN("failed to add column to stmt", K(ret));
          } else if (OB_UNLIKELY(column_exprs.empty()) || OB_ISNULL(column_exprs.at(0))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (0 == k) {
            ret = ObRawExprUtils::build_null_expr(*params.expr_factory_, new_value);
          } else if (1 == k) {
            ObConstRawExpr *state = NULL;
            ret = ObRawExprUtils::build_const_int_expr(*params.expr_factory_, ObIntType,
                                                       AI_COLUMN_STATE_PENDING, state);
            new_value = state;
          } else {
            new_value = request_expr;
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(ai_column_build_reset_expr(params, unchanged, column_exprs.at(0),
                                                        new_value, expr))) {
          } else {
            assign.column_expr_ = column_exprs.at(0);
            assign.expr_ = expr;
            assign.is_implicit_ = true;
            if (OB_FAIL(assignments.push_back(assign))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int AIColumnResolver::check_insert_values(ObResolverParams &params,
                                          const TableItem &table_item,
                                          const ObInsertStmt &stmt)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table_schema = NULL;
  AIColumnMeta meta;
  if (OB_ISNULL(params.session_info_) || OB_ISNULL(params.schema_checker_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (params.session_info_->is_inner() || !table_item.is_basic_table()) {
  } else if (OB_FAIL(params.schema_checker_->get_table_schema(table_item.ref_id_, table_schema))) {
    LOG_WARN("failed to get table schema", K(ret));
  } else if (OB_ISNULL(table_schema) || !table_schema->has_ai_column()) {
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(*table_schema, meta))) {
    LOG_WARN("failed to get ai column meta", K(ret));
  } else {
    const ObIArray<ObColumnRefRawExpr *> &desc = stmt.get_values_desc();
    const ObIArray<ObRawExpr *> &values = stmt.get_values_vector();
    for (int64_t i = 0; OB_SUCC(ret) && i < desc.count(); ++i) {
      const uint64_t column_id = OB_ISNULL(desc.at(i)) ? OB_INVALID_ID : desc.at(i)->get_column_id();
      bool allowed = true;
      if (!meta.is_ai_column(column_id) && !meta.is_hidden_column(column_id)) {
      } else if (stmt.value_from_select()) {
        allowed = false;
      } else {
        for (int64_t j = i; allowed && j < values.count(); j += desc.count()) {
          const ObRawExpr *value = values.at(j);
          allowed = NULL != value
              && (T_NULL == value->get_expr_type() || T_DEFAULT == value->get_expr_type()
                  || T_FUN_SYS_DEFAULT == value->get_expr_type()
                  || (value->is_const_raw_expr()
                      && static_cast<const ObConstRawExpr *>(value)->get_value().is_null()));
        }
      }
      if (!allowed) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "assigning a value to an AI column");
      }
    }
  }
  return ret;
}

static int ai_column_add_ref(ObResolverParams &params,
                             const uint64_t table_id,
                             const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table_schema = NULL;
  AIColumnMeta meta;
  ObIArray<uint64_t> &refs = params.query_ctx_->ai_column_refs_;
  bool found = false;
  for (int64_t i = 0; !found && i + 1 < refs.count(); i += 2) {
    found = refs.at(i) == table_id && refs.at(i + 1) == column_id;
  }
  if (found) {
  } else if (OB_FAIL(params.schema_checker_->get_table_schema(table_id, table_schema))) {
    LOG_WARN("failed to get table schema", K(ret), K(table_id));
  } else if (OB_ISNULL(table_schema) || !table_schema->has_ai_column()) {
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(*table_schema, meta))) {
    LOG_WARN("failed to get ai column meta", K(ret));
  } else if (!meta.is_ai_column(column_id)) {
  } else if (OB_FAIL(refs.push_back(table_id)) || OB_FAIL(refs.push_back(column_id))) {
  }
  return ret;
}

static int ai_column_collect_exprs(ObResolverParams &params,
                                   const ObDMLStmt &stmt,
                                   const ObIArray<ObRawExpr *> &exprs)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObRawExpr *, 8> columns;
  if (OB_FAIL(ObRawExprUtils::extract_column_exprs(exprs, columns))) {
    LOG_WARN("failed to extract column exprs", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    const ObColumnRefRawExpr *column = static_cast<const ObColumnRefRawExpr *>(columns.at(i));
    const TableItem *table = NULL;
    if (OB_ISNULL(column) || !column->is_column_ref_expr()) {
    } else if (OB_ISNULL(table = stmt.get_table_item_by_id(column->get_table_id()))
               || !table->is_basic_table()) {
    } else if (OB_FAIL(ai_column_add_ref(params, table->ref_id_, column->get_column_id()))) {
    }
  }
  return ret;
}

static int ai_column_collect_stmt(ObResolverParams &params, const ObDMLStmt &stmt)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObSelectStmt *, 4> child_stmts;
  if (stmt.is_select_stmt()) {
    // every column of a query block is referred to by the query text
    const ObIArray<ColumnItem> &column_items = stmt.get_column_items();
    for (int64_t i = 0; OB_SUCC(ret) && i < column_items.count(); ++i) {
      const TableItem *table = stmt.get_table_item_by_id(column_items.at(i).table_id_);
      if (NULL == table || !table->is_basic_table()) {
      } else if (OB_FAIL(ai_column_add_ref(params, table->ref_id_, column_items.at(i).column_id_))) {
      }
    }
  } else if (stmt.is_update_stmt() || stmt.is_delete_stmt()) {
    // the target table columns of UPDATE/DELETE include the ones written by the statement
    ObSEArray<ObRawExpr *, 8> exprs;
    if (OB_FAIL(append(exprs, stmt.get_condition_exprs()))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < stmt.get_order_items().count(); ++i) {
      ret = exprs.push_back(stmt.get_order_items().at(i).expr_);
    }
    if (OB_SUCC(ret) && stmt.is_update_stmt()) {
      const ObIArray<ObUpdateTableInfo *> &infos =
          static_cast<const ObUpdateStmt &>(stmt).get_update_table_info();
      for (int64_t i = 0; OB_SUCC(ret) && i < infos.count(); ++i) {
        for (int64_t j = 0; OB_SUCC(ret) && NULL != infos.at(i)
                            && j < infos.at(i)->assignments_.count(); ++j) {
          const ObAssignment &assign = infos.at(i)->assignments_.at(j);
          if (!assign.is_implicit_ && NULL != assign.expr_) {
            ret = exprs.push_back(assign.expr_);
          }
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(ai_column_collect_exprs(params, stmt, exprs))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(stmt.get_child_stmts(child_stmts))) {
    LOG_WARN("failed to get child stmts", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
    if (NULL != child_stmts.at(i)) {
      ret = SMART_CALL(ai_column_collect_stmt(params, *child_stmts.at(i)));
    }
  }
  return ret;
}

int AIColumnResolver::collect_column_refs(ObResolverParams &params, const ObDMLStmt &stmt)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(params.session_info_) || OB_ISNULL(params.query_ctx_)
      || OB_ISNULL(params.schema_checker_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (params.session_info_->is_inner()) {
  } else if (OB_FAIL(ai_column_collect_stmt(params, stmt))) {
    LOG_WARN("failed to collect ai column refs", K(ret));
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
