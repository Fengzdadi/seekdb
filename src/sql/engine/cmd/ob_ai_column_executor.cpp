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

#include "sql/engine/cmd/ob_ai_column_executor.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "lib/string/ob_sql_string.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{

namespace
{
int append_name(const ObString &name, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    const char c = name.ptr()[i];
    ret = '`' == c ? sql.append("``") : sql.append(&c, 1);
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("`"))) {
  }
  return ret;
}

int append_table_name(const ObString &database_name, const ObString &table_name, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_name(database_name, sql))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(append_name(table_name, sql))) {
  }
  return ret;
}

bool is_identifier_char(const char c)
{
  const unsigned char uc = static_cast<unsigned char>(c);
  return isalnum(uc) || '_' == c || '$' == c || uc >= 0x80;
}

int get_table_schema(ObSchemaGetterGuard &guard,
                     const ObString &database_name,
                     const ObString &table_name,
                     const ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;
  table_schema = NULL;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    LOG_WARN("failed to get schema guard", K(ret));
  } else if (OB_FAIL(guard.get_table_schema(database_name, table_name, false, table_schema))) {
    LOG_WARN("failed to get table schema", K(ret), K(database_name), K(table_name));
  } else if (OB_ISNULL(table_schema)) {
    ObCStringHelper helper;
    ret = OB_TABLE_NOT_EXIST;
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, helper.convert(database_name), helper.convert(table_name));
  }
  return ret;
}

// whether some row of the AI column is not READY, NULL states belong to rows that existed
// before the column was added and have not been backfilled yet
int has_unready_row(const ObString &database_name,
                    const ObString &table_name,
                    const ObString &state_column_name,
                    bool &found)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  found = false;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is null", K(ret));
  } else if (OB_FAIL(sql.assign("SELECT /*+ opt_param('hidden_column_visible', 'true') */ 1 FROM "))) {
  } else if (OB_FAIL(append_table_name(database_name, table_name, sql))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(append_name(state_column_name, sql))) {
  } else if (OB_FAIL(sql.append(" IS NULL OR "))) {
  } else if (OB_FAIL(append_name(state_column_name, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" <> %d LIMIT 1", static_cast<int>(AI_COLUMN_STATE_READY)))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("failed to read ai column states", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", K(ret));
      } else if (OB_SUCC(result->next())) {
        found = true;
      } else if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("failed to get next row", K(ret));
      }
    }
  }
  return ret;
}
} // namespace

int AIColumnExecutor::check_strict_read(ObSQLSessionInfo &session, const ObPhysicalPlan &plan)
{
  int ret = OB_SUCCESS;
  const ObIArray<uint64_t> &refs = plan.get_ai_column_refs();
  bool strict_read = false;
  if (refs.empty() || session.is_inner()) {
  } else if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, strict_read))) {
    LOG_WARN("failed to get ai_column_strict_read", K(ret));
  } else if (!strict_read) {
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else {
    ObSchemaGetterGuard guard;
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
      LOG_WARN("failed to get schema guard", K(ret));
    }
    // refs holds (table id, column id) pairs
    for (int64_t i = 0; OB_SUCC(ret) && i + 1 < refs.count(); i += 2) {
      const uint64_t table_id = refs.at(i);
      const uint64_t column_id = refs.at(i + 1);
      const ObTableSchema *table_schema = NULL;
      const ObDatabaseSchema *database_schema = NULL;
      AIColumnMeta meta;
      const AIColumnDef *def = NULL;
      bool found = false;
      if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
        LOG_WARN("failed to get table schema", K(ret), K(table_id));
      } else if (OB_ISNULL(table_schema)) {
        // dropped after the plan was generated, the plan itself will fail
      } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(*table_schema, meta))) {
        LOG_WARN("failed to get ai column meta", K(ret), K(table_id));
      } else if (OB_ISNULL(def = meta.get_def(column_id))) {
      } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(),
                                                   database_schema))) {
        LOG_WARN("failed to get database schema", K(ret));
      } else if (OB_ISNULL(database_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("database schema is null", K(ret));
      } else if (OB_FAIL(has_unready_row(database_schema->get_database_name_str(),
                                         table_schema->get_table_name_str(),
                                         def->state_column_name_, found))) {
      } else if (found) {
        ret = OB_AI_COLUMN_NOT_READY;
        LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, static_cast<int64_t>(table_id),
                       static_cast<int64_t>(column_id));
      }
    }
  }
  return ret;
}

int AIColumnExecutor::rewrite_retry_condition(const ObString &condition,
                                              const ObString &state_column_name,
                                              ObSqlString &rewritten)
{
  int ret = OB_SUCCESS;
  const char *str = condition.ptr();
  const int64_t len = condition.length();
  int64_t pos = 0;
  rewritten.reuse();
  while (OB_SUCC(ret) && pos < len) {
    const char c = str[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      // copy a quoted literal or name as it is
      int64_t end = pos + 1;
      bool closed = false;
      while (!closed && end < len) {
        if ('\\' == str[end] && '`' != c && end + 1 < len) {
          end += 2;
        } else if (c == str[end] && end + 1 < len && c == str[end + 1]) {
          end += 2;
        } else {
          closed = c == str[end];
          ++end;
        }
      }
      ret = rewritten.append(str + pos, end - pos);
      pos = end;
    } else if (is_identifier_char(c)) {
      int64_t end = pos;
      while (end < len && is_identifier_char(str[end])) {
        ++end;
      }
      const ObString word(static_cast<int32_t>(end - pos), str + pos);
      bool is_status = 0 == word.case_compare("status");
      if (is_status) {
        int64_t before = pos - 1;
        int64_t after = end;
        while (before >= 0 && isspace(static_cast<unsigned char>(str[before]))) {
          --before;
        }
        while (after < len && isspace(static_cast<unsigned char>(str[after]))) {
          ++after;
        }
        is_status = !(before >= 0 && '.' == str[before])
                    && !(after < len && ('.' == str[after] || '(' == str[after]));
      }
      if (!is_status) {
        ret = rewritten.append(word);
      } else if (OB_FAIL(rewritten.append("(CASE "))) {
      } else if (OB_FAIL(append_name(state_column_name, rewritten))) {
      } else if (OB_FAIL(rewritten.append_fmt(" WHEN %d THEN 'READY' WHEN %d THEN 'FAILED'"
                                              " ELSE 'PENDING' END)",
                                              static_cast<int>(AI_COLUMN_STATE_READY),
                                              static_cast<int>(AI_COLUMN_STATE_FAILED)))) {
      }
      pos = end;
    } else {
      ret = rewritten.append(str + pos, 1);
      ++pos;
    }
  }
  return ret;
}

int AIColumnExecutor::invalidate(ObSQLSessionInfo &session,
                                 const ObString &database_name,
                                 const ObString &table_name,
                                 const ObIArray<ObString> &column_names,
                                 const ObString &condition)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  const ObTableSchema *table_schema = NULL;
  AIColumnMeta meta;
  ObSqlString sql;
  ObSqlString rewritten;
  int64_t affected_rows = 0;
  UNUSED(session);
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is null", K(ret));
  } else if (OB_FAIL(get_table_schema(guard, database_name, table_name, table_schema))) {
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(*table_schema, meta))) {
    LOG_WARN("failed to get ai column meta", K(ret));
  } else if (OB_FAIL(sql.assign("UPDATE /*+ opt_param('hidden_column_visible', 'true') */ "))) {
  } else if (OB_FAIL(append_table_name(database_name, table_name, sql))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  }
  // a fresh request id per row hands the row to the AI column worker, the DML task hook
  // replaces the pending task of every AI column whose request id changes
  for (int64_t i = 0; OB_SUCC(ret) && i < column_names.count(); ++i) {
    const AIColumnDef *def = meta.get_def_by_name(column_names.at(i));
    if (OB_ISNULL(def)) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
    } else if (i > 0 && OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_name(def->column_name_, sql))) {
    } else if (OB_FAIL(sql.append(" = NULL, "))) {
    } else if (OB_FAIL(append_name(def->state_column_name_, sql))) {
    } else if (OB_FAIL(sql.append_fmt(" = %d, ", static_cast<int>(AI_COLUMN_STATE_PENDING)))) {
    } else if (OB_FAIL(append_name(def->request_column_name_, sql))) {
    } else if (OB_FAIL(sql.append(" = uuid_short()"))) {
    } else if (!condition.empty() && 0 == i
               && OB_FAIL(rewrite_retry_condition(condition, def->state_column_name_, rewritten))) {
      LOG_WARN("failed to rewrite retry condition", K(ret), K(condition));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!condition.empty() && OB_FAIL(sql.append_fmt(" WHERE (%.*s)",
                                                         static_cast<int>(rewritten.length()),
                                                         rewritten.ptr()))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to invalidate ai column", K(ret), K(sql));
  } else {
    LOG_INFO("ai column rows invalidated", K(database_name), K(table_name), K(column_names),
             K(affected_rows));
  }
  return ret;
}

int AIColumnExecutor::retry(ObSQLSessionInfo &session,
                            const ObString &database_name,
                            const ObString &table_name,
                            const ObString &column_name,
                            const ObString &condition)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObString, 1> column_names;
  if (OB_FAIL(column_names.push_back(column_name))) {
  } else if (OB_FAIL(invalidate(session, database_name, table_name, column_names, condition))) {
  }
  return ret;
}

int AIColumnExecutor::refresh(ObSQLSessionInfo &session,
                              const ObString &database_name,
                              const ObString &table_name,
                              const ObIArray<ObString> &column_names)
{
  int ret = OB_SUCCESS;
  if (column_names.empty()) {
  } else if (OB_FAIL(invalidate(session, database_name, table_name, column_names, ObString()))) {
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
