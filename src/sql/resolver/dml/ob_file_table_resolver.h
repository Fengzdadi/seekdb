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

// Resolver side of file SQL: read_csv / read_json / read_parquet / glob table functions,
// DESCRIBE SELECT over them and the options of COPY ... TO.

#pragma once

#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/parser/parse_node.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;
class ObRawExprResType;
class ObStmt;
class ObSelectStmt;
class ObQueryCtx;
struct ObResolverParams;

class FileTableResolver
{
public:
  // enable_file_sql must be ON in the session.
  static int check_enabled(const ObSQLSessionInfo &session);
  // Latest global secure_file_priv.
  static int get_secure_file_priv(common::ObIAllocator &alloc, common::ObString &secure_file_priv);
  // Resolve a T_FILE_TABLE_EXPRESSION node into an inferred descriptor.
  static int resolve_file_table(const ParseNode &node, const ObSQLSessionInfo &session,
                                common::ObIAllocator &alloc, FileTableDesc *&desc,
                                common::ObString &alias_name);
  static int get_column_type(const FileColumnDesc &column, const ObSQLSessionInfo &session,
                             ObRawExprResType &type);
  // Resolve the T_COPY_INTO node of COPY ... TO 'path' (options).
  static int resolve_copy_into(const ParseNode &node, const ObSQLSessionInfo &session,
                               common::ObIAllocator &alloc, common::ObString &path,
                               CsvExportOptions &options);
  // Once the select list is resolved, record its column names for the CSV header.
  static int finish_copy_into(ObSelectStmt &stmt, common::ObIAllocator &alloc);
  // DESCRIBE SELECT ... FROM read_*(...): resolve the query to learn its result columns
  // and return a select statement listing them.
  static int resolve_describe(ObResolverParams &params, const ParseNode &parse_tree,
                              ObStmt *&stmt);
  // File tables are inferred at every execution, so their plans are never cached.
  static void disable_plan_cache(ObQueryCtx *query_ctx);
  static bool is_file_table_func(const common::ObString &name, FileFormat &format);
private:
  static int resolve_options(const ParseNode *option_list, common::ObIAllocator &alloc,
                             FileTableDesc &desc);
};

} // namespace sql
} // namespace oceanbase
