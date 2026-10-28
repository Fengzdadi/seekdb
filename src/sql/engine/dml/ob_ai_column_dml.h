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

#include "lib/container/ob_fixed_array.h"
#include "lib/list/ob_list.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/ob_unify_serialize.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObTableSchema;
}
}
namespace sql
{
class ObTableModifyOp;
struct ObDMLModifyRowNode;

// What a DML operator needs to keep __all_ai_column_task in step with the rows it writes.
// Only the primary table DML of a table with AI columns carries a valid one. Row positions
// refer to old_row_/new_row_ of the owning ctdef (both follow its column_ids_) or, for the
// new values of an UPDATE, to its full_row_.
struct AIColumnDMLCtDef
{
  OB_UNIS_VERSION(1);
public:
  explicit AIColumnDMLCtDef(common::ObIAllocator &alloc)
    : table_id_(common::OB_INVALID_ID),
      rowkey_names_(alloc),
      rowkey_idxs_(alloc),
      rowkey_full_idxs_(alloc),
      column_ids_(alloc),
      max_retries_(alloc),
      definition_versions_(alloc),
      request_idxs_(alloc),
      request_full_idxs_(alloc)
  {}
  bool is_valid() const { return common::OB_INVALID_ID != table_id_; }
  // dml_column_ids: column ids of the ctdef rows
  // assign_idxs: UPDATE only, the row position of each assigned column in assignment order
  int init(common::ObIAllocator &allocator,
           const share::schema::ObTableSchema &table_schema,
           const common::ObIArray<uint64_t> &dml_column_ids,
           const common::ObIArray<int64_t> *assign_idxs);
  TO_STRING_KV(K_(table_id), K_(rowkey_names), K_(rowkey_idxs), K_(rowkey_full_idxs),
               K_(column_ids), K_(max_retries), K_(definition_versions), K_(request_idxs),
               K_(request_full_idxs));

  uint64_t table_id_;
  common::ObFixedArray<common::ObString, common::ObIAllocator> rowkey_names_;
  common::ObFixedArray<int64_t, common::ObIAllocator> rowkey_idxs_;
  common::ObFixedArray<int64_t, common::ObIAllocator> rowkey_full_idxs_;
  common::ObFixedArray<uint64_t, common::ObIAllocator> column_ids_;
  common::ObFixedArray<int64_t, common::ObIAllocator> max_retries_;
  common::ObFixedArray<int64_t, common::ObIAllocator> definition_versions_;
  // UPDATE only, -1 when the statement does not assign the request column of that AI column
  common::ObFixedArray<int64_t, common::ObIAllocator> request_idxs_;
  common::ObFixedArray<int64_t, common::ObIAllocator> request_full_idxs_;
};

// Turns the rows written by a DML operator into task changes in the same transaction:
// inserted rows get one task per AI column, updated rows whose request column changed get
// a task for each such column, deleted rows (and old keys of updated rows) lose their tasks.
// The AI columns invalidated together by one row change share one request id.
class AIColumnDMLHandler
{
public:
  static int handle(ObTableModifyOp &op,
                    common::ObList<ObDMLModifyRowNode, common::ObIAllocator> &rows);
};

} // namespace sql
} // namespace oceanbase
