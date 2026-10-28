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

#pragma once

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/ob_unify_serialize.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
class ObISQLClient;
class ObSqlString;
}
namespace share
{
namespace schema
{
class ObTableSchema;
class ObColumnSchemaV2;

// Value kept in the hidden state column of an AI column. A NULL state is treated as PENDING.
enum AIColumnValueState
{
  AI_COLUMN_STATE_PENDING = 0,
  AI_COLUMN_STATE_READY = 1,
  AI_COLUMN_STATE_FAILED = 2,
};

// State of a row in __all_ai_column_task.
enum AIColumnTaskState
{
  AI_TASK_STATE_PENDING = 0,
  AI_TASK_STATE_RUNNING = 1,
  AI_TASK_STATE_SUCCEEDED = 2,
  AI_TASK_STATE_RETRY_WAIT = 3,
  AI_TASK_STATE_FAILED = 4,
};

enum AIColumnFunction
{
  AI_COLUMN_FUNC_INVALID = 0,
  AI_COLUMN_FUNC_COMPLETE = 1,
  AI_COLUMN_FUNC_EMBED = 2,
};

// Definition of one AI column. Column ids are filled in by the DDL service once the final
// column ids of the table are known; before that the definition refers to columns by name.
struct AIColumnDef
{
  OB_UNIS_VERSION(1);
public:
  AIColumnDef() { reset(); }
  void reset();
  bool is_valid() const;
  bool is_complete() const { return AI_COLUMN_FUNC_COMPLETE == function_type_; }
  bool is_embed() const { return AI_COLUMN_FUNC_EMBED == function_type_; }
  bool depends_on(const uint64_t column_id) const;
  bool is_hidden_column(const uint64_t column_id) const
  {
    return column_id == state_column_id_ || column_id == request_column_id_
        || column_id == spec_column_id_;
  }
  const char *get_function_name() const;
  // shallow copy, strings keep pointing to the memory of other
  int assign(const AIColumnDef &other);
  int deep_copy(const AIColumnDef &other, common::ObIAllocator &allocator);

  TO_STRING_KV(K_(column_id), K_(column_name), K_(state_column_id), K_(request_column_id),
               K_(spec_column_id), K_(function_type), K_(model_name), K_(prompt_expr),
               K_(type_prompt), K_(config), K_(dependency_column_ids), K_(result_type),
               K_(vector_dimension), K_(max_retries), K_(definition_version));

  uint64_t column_id_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t spec_column_id_;
  common::ObString column_name_;
  common::ObString state_column_name_;
  common::ObString request_column_name_;
  common::ObString spec_column_name_;
  int64_t function_type_;
  common::ObString model_name_;
  // the AI expression as written by the user
  common::ObString expr_str_;
  // SQL text of the prompt (AI_COMPLETE) or input (AI_EMBED) argument
  common::ObString prompt_expr_;
  // the expression after the result type prompt has been added
  common::ObString normalized_expr_;
  // system prompt derived from the result column type
  common::ObString type_prompt_;
  // JSON request options (AI_COMPLETE config argument, AI_EMBED dimensions)
  common::ObString config_;
  common::ObSEArray<uint64_t, 4> dependency_column_ids_;
  common::ObSEArray<common::ObString, 4> dependency_column_names_;
  common::ObString result_type_;
  int64_t vector_dimension_;
  int64_t max_retries_;
  int64_t definition_version_;
};

// All AI column definitions of one table. It is kept in serialized form in ObTableSchema.
struct AIColumnMeta
{
  OB_UNIS_VERSION(1);
public:
  AIColumnMeta() : defs_() {}
  void reset() { defs_.reset(); }
  int assign(const AIColumnMeta &other) { return defs_.assign(other.defs_); }
  int deep_copy(const AIColumnMeta &other, common::ObIAllocator &allocator);
  bool empty() const { return defs_.empty(); }
  int64_t count() const { return defs_.count(); }
  const AIColumnDef *get_def(const uint64_t column_id) const;
  AIColumnDef *get_def(const uint64_t column_id);
  const AIColumnDef *get_def_by_name(const common::ObString &column_name) const;
  const AIColumnDef *get_def_by_hidden_column(const uint64_t column_id) const;
  bool is_ai_column(const uint64_t column_id) const { return NULL != get_def(column_id); }
  bool is_hidden_column(const uint64_t column_id) const
  {
    return NULL != get_def_by_hidden_column(column_id);
  }
  bool is_dependency_column(const uint64_t column_id) const;
  int remove_def(const uint64_t column_id);

  TO_STRING_KV(K_(defs));

  common::ObSEArray<AIColumnDef, 2> defs_;
};

class AIColumnSchemaUtil
{
public:
  static const int64_t DEFAULT_MAX_RETRIES = 3;
  static const int64_t MAX_MAX_RETRIES = 100;
  static constexpr const char *STATE_COLUMN_PREFIX = "__ai_state_";
  static constexpr const char *REQUEST_COLUMN_PREFIX = "__ai_request_";
  static constexpr const char *SPEC_COLUMN_PREFIX = "__ai_spec_";

  // decoded strings point into the schema, so the schema must outlive the meta
  static int get_meta(const ObTableSchema &table_schema, AIColumnMeta &meta);
  static int set_meta(const AIColumnMeta &meta, ObTableSchema &table_schema);
  static int decode(const common::ObString &data, AIColumnMeta &meta);
  static int encode(const AIColumnMeta &meta, common::ObIAllocator &allocator,
                    common::ObString &data);
  // fill column ids of every definition from the column names in table_schema
  static int fill_column_ids(const ObTableSchema &table_schema, AIColumnMeta &meta);
  // a name with the given prefix that is used neither in table_schema nor among the
  // columns added by the same ALTER TABLE statement (added_columns, may be NULL)
  static int build_hidden_column_name(const char *prefix,
                                      const common::ObString &ai_column_name,
                                      const ObTableSchema &table_schema,
                                      const ObTableSchema *added_columns,
                                      common::ObIAllocator &allocator,
                                      common::ObString &name);
  // look up a column by name with a linear scan, which also works for ALTER TABLE schemas
  static const ObColumnSchemaV2 *find_column(const ObTableSchema &table_schema,
                                             const common::ObString &name);
  // system prompt asking the model to answer in a form that converts to the column type
  static int build_type_prompt(const ObColumnSchemaV2 &column,
                               common::ObIAllocator &allocator,
                               common::ObString &prompt);
  static int build_result_type_str(const ObColumnSchemaV2 &column,
                                   common::ObIAllocator &allocator,
                                   common::ObString &type_str);
  static const char *get_state_name(const int64_t state);
};

// Persistence of AI column definitions in __all_ai_column and cleanup of
// __all_ai_column_task. All functions run inside the caller's transaction.
class AIColumnSqlService
{
public:
  static int insert_defs(common::ObISQLClient &sql_client,
                         const uint64_t table_id,
                         const AIColumnMeta &meta);
  static int delete_defs(common::ObISQLClient &sql_client, const uint64_t table_id);
  static int replace_defs(common::ObISQLClient &sql_client,
                          const uint64_t table_id,
                          const AIColumnMeta &meta);
  // delete task rows of a table, or of one column when column_id is valid
  static int delete_tasks(common::ObISQLClient &sql_client,
                          const uint64_t table_id,
                          const uint64_t column_id);
  // load definitions of the given tables and attach them to the matching schemas
  static int fetch_and_attach(common::ObISQLClient &sql_client,
                              const common::ObIArray<uint64_t> &table_ids,
                              common::ObIArray<ObTableSchema *> &table_schemas);
  // attach definitions to every user table among table_schemas
  static int attach_to_user_tables(common::ObISQLClient &sql_client,
                                   common::ObIArray<ObTableSchema *> &table_schemas);

  // DDL service hooks
  static int on_create_tables(common::ObISQLClient &sql_client,
                              common::ObIArray<ObTableSchema> &tables,
                              const bool is_truncate_table);
  static int on_drop_table(common::ObISQLClient &sql_client, const ObTableSchema &table);
  // merge the definitions resolved by ALTER TABLE (carried by alter_table) into new_table
  static int on_alter_table(common::ObISQLClient &sql_client,
                            const ObTableSchema &orig_table,
                            const ObTableSchema &alter_table,
                            ObTableSchema &new_table);
};

} // namespace schema
} // namespace share
} // namespace oceanbase
