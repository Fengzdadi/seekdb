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

#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "sql/resolver/ob_resolver_define.h"

namespace oceanbase
{
namespace sql
{
class ObAlterTableStmt;
class ObDMLResolver;
class ObDMLStmt;
class ObInsertStmt;
struct ObAssignment;
struct TableItem;

// Columns an AI column definition may refer to while it is being resolved.
struct AIColumnScope
{
  AIColumnScope() : table_name_(), table_(NULL), added_(NULL), ai_names_() {}
  const share::schema::ObColumnSchemaV2 *get_column(const common::ObString &name) const;
  bool is_ai_column(const common::ObString &name) const;

  common::ObString table_name_;
  // the table being created, or the original table for ALTER TABLE
  const share::schema::ObTableSchema *table_;
  // columns added by the same ALTER TABLE statement
  const share::schema::ObTableSchema *added_;
  // AI columns of the table, including the ones defined by the current statement
  common::ObSEArray<common::ObString, 4> ai_names_;
};

class AIColumnResolver
{
public:
  enum AlterKind
  {
    ALTER_MODIFY = 0,
    ALTER_RENAME,
    ALTER_DROP,
  };

  // the T_AI_COLUMN attribute of a column definition, NULL for an ordinary column
  static const ParseNode *get_ai_column_node(const ParseNode *column_definition);
  static bool has_ai_column(const ParseNode *table_element_list);

  // CREATE TABLE: resolve the AI columns of the element list, add their hidden columns
  // (allocating ids from cur_column_id) and store the definitions in table_schema.
  static int resolve_create_table(ObResolverParams &params,
                                  const ParseNode &table_element_list,
                                  const common::ObString &table_name,
                                  const bool has_primary_key,
                                  share::schema::ObTableSchema &table_schema,
                                  uint64_t &cur_column_id);

  // ALTER TABLE ADD/MODIFY COLUMN with an AI COLUMN clause. The definition is added to the
  // AI meta carried by the ALTER TABLE schema; ids are filled in by the DDL service.
  static int resolve_alter_column(ObResolverParams &params,
                                  const ParseNode &ai_node,
                                  const share::schema::ObTableSchema &orig_table,
                                  const share::schema::ObColumnSchemaV2 &column,
                                  const bool is_modify,
                                  ObAlterTableStmt &stmt);

  // ALTER TABLE ... RETRY AI COLUMN col [WHERE cond]
  static int resolve_retry(ObResolverParams &params,
                           const ParseNode &node,
                           const int64_t action_count,
                           const share::schema::ObTableSchema &table,
                           ObAlterTableStmt &stmt);

  // ALTER TABLE MODIFY/CHANGE/RENAME/DROP of a column must keep AI column definitions valid
  static int check_alter_column_allowed(const share::schema::ObTableSchema &orig_table,
                                        const common::ObString &column_name,
                                        const AlterKind kind,
                                        const bool has_ai_clause);

  // UPDATE and INSERT ... ON DUPLICATE KEY UPDATE of a table with AI columns: business SQL must
  // not assign AI columns, and an AI column whose dependencies or primary key get a different
  // value is reset to PENDING with a new request id in the same row write.
  static int resolve_dml_assignments(ObDMLResolver &resolver,
                                     const TableItem &table_item,
                                     const share::schema::ObTableSchema &table_schema,
                                     common::ObIArray<ObAssignment> &assignments);
  // INSERT and REPLACE must not give AI columns a value other than NULL or DEFAULT
  static int check_insert_values(ObResolverParams &params,
                                 const TableItem &table_item,
                                 const ObInsertStmt &stmt);
  // record the AI columns referred to by the statement for ai_column_strict_read
  static int collect_column_refs(ObResolverParams &params, const ObDMLStmt &stmt);

private:
  static int resolve_definition(ObResolverParams &params,
                                const ParseNode &ai_node,
                                const share::schema::ObColumnSchemaV2 &column,
                                const AIColumnScope &scope,
                                common::ObIAllocator &allocator,
                                share::schema::AIColumnDef &def);
  static int resolve_options(const ParseNode *options, share::schema::AIColumnDef &def);
  static int check_result_type(const share::schema::ObColumnSchemaV2 &column,
                               share::schema::AIColumnDef &def);
  static int build_hidden_column(const common::ObString &name,
                                 const common::ObObjType type,
                                 share::schema::ObColumnSchemaV2 &column);
  static int get_vector_dimension(const share::schema::ObColumnSchemaV2 &column, int64_t &dim);
  static int build_normalized_expr(const share::schema::AIColumnDef &def,
                                   common::ObIAllocator &allocator,
                                   common::ObString &expr);
};

} // namespace sql
} // namespace oceanbase
