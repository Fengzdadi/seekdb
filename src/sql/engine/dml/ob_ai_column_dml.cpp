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

#include "sql/engine/dml/ob_ai_column_dml.h"
#include "lib/string/ob_sql_string.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/cmd/ob_ai_column_executor.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_uuid_short.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{

OB_SERIALIZE_MEMBER(AIColumnDMLCtDef,
                    table_id_,
                    rowkey_names_,
                    rowkey_idxs_,
                    rowkey_full_idxs_,
                    column_ids_,
                    max_retries_,
                    definition_versions_,
                    request_idxs_,
                    request_full_idxs_);

static int64_t ai_column_find_idx(const ObIArray<uint64_t> &column_ids, const uint64_t column_id)
{
  int64_t idx = -1;
  for (int64_t i = 0; -1 == idx && i < column_ids.count(); ++i) {
    if (column_ids.at(i) == column_id) {
      idx = i;
    }
  }
  return idx;
}

// position of the new value of the column at row position idx in full_row_, -1 if not assigned
static int64_t ai_column_find_full_idx(const ObIArray<int64_t> &assign_idxs,
                                       const int64_t row_cnt,
                                       const int64_t idx)
{
  int64_t full_idx = -1;
  for (int64_t i = 0; -1 == full_idx && i < assign_idxs.count(); ++i) {
    if (assign_idxs.at(i) == idx) {
      full_idx = row_cnt + i;
    }
  }
  return full_idx;
}

int AIColumnDMLCtDef::init(ObIAllocator &allocator,
                           const ObTableSchema &table_schema,
                           const ObIArray<uint64_t> &dml_column_ids,
                           const ObIArray<int64_t> *assign_idxs)
{
  int ret = OB_SUCCESS;
  AIColumnMeta meta;
  ObSEArray<uint64_t, 4> rowkey_column_ids;
  const int64_t row_cnt = dml_column_ids.count();
  bool need_task = (NULL == assign_idxs);
  table_id_ = OB_INVALID_ID;
  if (!table_schema.has_ai_column()) {
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(table_schema, meta))) {
    LOG_WARN("failed to get ai column meta", K(ret));
  } else if (OB_FAIL(table_schema.get_rowkey_info().get_column_ids(rowkey_column_ids))) {
    LOG_WARN("failed to get rowkey column ids", K(ret));
  } else if (OB_FAIL(rowkey_names_.init(rowkey_column_ids.count()))
             || OB_FAIL(rowkey_idxs_.init(rowkey_column_ids.count()))
             || OB_FAIL(rowkey_full_idxs_.init(rowkey_column_ids.count()))
             || OB_FAIL(column_ids_.init(meta.count()))
             || OB_FAIL(max_retries_.init(meta.count()))
             || OB_FAIL(definition_versions_.init(meta.count()))
             || OB_FAIL(request_idxs_.init(meta.count()))
             || OB_FAIL(request_full_idxs_.init(meta.count()))) {
    LOG_WARN("failed to init arrays", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_column_ids.count(); ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema(rowkey_column_ids.at(i));
    const int64_t idx = ai_column_find_idx(dml_column_ids, rowkey_column_ids.at(i));
    ObString name;
    if (OB_ISNULL(column) || OB_UNLIKELY(idx < 0)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("rowkey column is not written by dml", K(ret), K(rowkey_column_ids), K(dml_column_ids));
    } else if (OB_FAIL(ob_write_string(allocator, column->get_column_name_str(), name))) {
    } else if (OB_FAIL(rowkey_names_.push_back(name))) {
    } else if (OB_FAIL(rowkey_idxs_.push_back(idx))) {
    } else {
      int64_t full_idx = idx;
      if (NULL != assign_idxs) {
        full_idx = ai_column_find_full_idx(*assign_idxs, row_cnt, idx);
        full_idx = full_idx < 0 ? idx : full_idx;
      }
      if (OB_FAIL(rowkey_full_idxs_.push_back(full_idx))) {
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < meta.count(); ++i) {
    const AIColumnDef &def = meta.defs_.at(i);
    int64_t request_idx = -1;
    int64_t request_full_idx = -1;
    if (NULL != assign_idxs) {
      request_idx = ai_column_find_idx(dml_column_ids, def.request_column_id_);
      if (request_idx >= 0) {
        request_full_idx = ai_column_find_full_idx(*assign_idxs, row_cnt, request_idx);
      }
      if (request_full_idx < 0) {
        request_idx = -1;
      } else {
        need_task = true;
      }
    }
    if (OB_FAIL(column_ids_.push_back(def.column_id_))) {
    } else if (OB_FAIL(max_retries_.push_back(def.max_retries_))) {
    } else if (OB_FAIL(definition_versions_.push_back(def.definition_version_))) {
    } else if (OB_FAIL(request_idxs_.push_back(request_idx))) {
    } else if (OB_FAIL(request_full_idxs_.push_back(request_full_idx))) {
    }
  }
  if (OB_SUCC(ret) && table_schema.has_ai_column() && need_task && !meta.empty()) {
    table_id_ = table_schema.get_table_id();
  }
  return ret;
}

namespace
{
// task changes of one table, flushed as one DELETE and one REPLACE statement
struct AITaskBatch
{
  AITaskBatch() : table_id_(OB_INVALID_ID), delete_cnt_(0), replace_cnt_(0), deletes_(), replaces_() {}
  uint64_t table_id_;
  int64_t delete_cnt_;
  int64_t replace_cnt_;
  ObSqlString deletes_;
  ObSqlString replaces_;
};

class AITaskWriter
{
public:
  static const int64_t FLUSH_SIZE = 512 * 1024;

  explicit AITaskWriter(ObTableModifyOp &op)
    : op_(op), allocator_("AIColumnDML"), batches_() {}
  ~AITaskWriter()
  {
    for (int64_t i = 0; i < batches_.count(); ++i) {
      if (NULL != batches_.at(i)) {
        batches_.at(i)->~AITaskBatch();
      }
    }
  }
  int add_row(const ObDMLModifyRowNode &node);
  int flush_all();

private:
  int get_batch(const uint64_t table_id, AITaskBatch *&batch);
  int build_predicate(const AIColumnDMLCtDef &ai_ctdef,
                      const ExprFixedArray &exprs,
                      const ObChunkDatumStore::StoredRow &row,
                      const ObIArray<int64_t> &idxs,
                      ObSqlString &predicate);
  int append_literal(const ObString &str, ObSqlString &sql);
  int add_delete(AITaskBatch &batch, const ObString &predicate);
  int add_task(AITaskBatch &batch,
               const AIColumnDMLCtDef &ai_ctdef,
               const int64_t column_idx,
               const ObString &predicate,
               const uint64_t request_id);
  int flush(AITaskBatch &batch);
  static bool is_request_changed(const ObDatum &old_datum, const ObDatum &new_datum);

  ObTableModifyOp &op_;
  ObArenaAllocator allocator_;
  ObSEArray<AITaskBatch *, 2> batches_;
};

bool AITaskWriter::is_request_changed(const ObDatum &old_datum, const ObDatum &new_datum)
{
  bool changed = false;
  if (old_datum.is_null() || new_datum.is_null()) {
    changed = old_datum.is_null() != new_datum.is_null();
  } else {
    changed = old_datum.get_uint() != new_datum.get_uint();
  }
  return changed;
}

int AITaskWriter::get_batch(const uint64_t table_id, AITaskBatch *&batch)
{
  int ret = OB_SUCCESS;
  batch = NULL;
  for (int64_t i = 0; NULL == batch && i < batches_.count(); ++i) {
    if (batches_.at(i)->table_id_ == table_id) {
      batch = batches_.at(i);
    }
  }
  if (NULL == batch) {
    void *buf = allocator_.alloc(sizeof(AITaskBatch));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc task batch", K(ret));
    } else {
      batch = new (buf) AITaskBatch();
      batch->table_id_ = table_id;
      if (OB_FAIL(batches_.push_back(batch))) {
        batch->~AITaskBatch();
        batch = NULL;
      }
    }
  }
  return ret;
}

int AITaskWriter::append_literal(const ObString &str, ObSqlString &sql)
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
      ret = sql.append(&c, 1);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
  }
  return ret;
}

int AITaskWriter::build_predicate(const AIColumnDMLCtDef &ai_ctdef,
                                  const ExprFixedArray &exprs,
                                  const ObChunkDatumStore::StoredRow &row,
                                  const ObIArray<int64_t> &idxs,
                                  ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  predicate.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < idxs.count(); ++i) {
    const int64_t idx = idxs.at(i);
    ObObj obj;
    char *buf = NULL;
    int64_t len = 0;
    int64_t pos = 0;
    ObArenaAllocator tmp_allocator("AIColumnDML");
    if (OB_UNLIKELY(idx < 0 || idx >= row.cnt_ || idx >= exprs.count())
        || OB_ISNULL(exprs.at(idx))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid rowkey position", K(ret), K(idx), K(row.cnt_), K(exprs.count()));
    } else if (OB_FAIL(row.cells()[idx].to_obj(obj, exprs.at(idx)->obj_meta_))) {
      LOG_WARN("failed to convert rowkey datum", K(ret));
    } else if (OB_FAIL(obj.print_sql_literal(buf, len, pos, tmp_allocator,
                                             op_.get_obj_print_params()))) {
      LOG_WARN("failed to print rowkey value", K(ret));
    } else if (OB_FAIL(predicate.append_fmt("%s`%.*s` = %.*s", 0 == i ? "" : " and ",
                                            ai_ctdef.rowkey_names_.at(i).length(),
                                            ai_ctdef.rowkey_names_.at(i).ptr(),
                                            static_cast<int>(pos), buf))) {
    }
  }
  return ret;
}

int AITaskWriter::add_delete(AITaskBatch &batch, const ObString &predicate)
{
  int ret = OB_SUCCESS;
  if (batch.delete_cnt_ > 0 && OB_FAIL(batch.deletes_.append(", "))) {
  } else if (OB_FAIL(append_literal(predicate, batch.deletes_))) {
  } else {
    ++batch.delete_cnt_;
  }
  return ret;
}

int AITaskWriter::add_task(AITaskBatch &batch,
                           const AIColumnDMLCtDef &ai_ctdef,
                           const int64_t column_idx,
                           const ObString &predicate,
                           const uint64_t request_id)
{
  int ret = OB_SUCCESS;
  if (batch.replace_cnt_ > 0 && OB_FAIL(batch.replaces_.append(", "))) {
  } else if (OB_FAIL(batch.replaces_.append_fmt("(%lu, ", batch.table_id_))) {
  } else if (OB_FAIL(append_literal(predicate, batch.replaces_))) {
  } else if (OB_FAIL(batch.replaces_.append_fmt(
                 ", %lu, %lu, %ld, %d, 0, %ld, now(6), NULL, NULL, NULL, NULL)",
                 ai_ctdef.column_ids_.at(column_idx), request_id,
                 ai_ctdef.definition_versions_.at(column_idx),
                 static_cast<int>(AI_TASK_STATE_PENDING),
                 ai_ctdef.max_retries_.at(column_idx)))) {
  } else {
    ++batch.replace_cnt_;
  }
  return ret;
}

int AITaskWriter::add_row(const ObDMLModifyRowNode &node)
{
  int ret = OB_SUCCESS;
  const ObDMLBaseCtDef &ctdef = *node.dml_ctdef_;
  const AIColumnDMLCtDef &ai_ctdef = ctdef.ai_ctdef_;
  AITaskBatch *batch = NULL;
  ObSqlString old_predicate;
  ObSqlString new_predicate;
  bool need_delete = false;
  // column positions in ai_ctdef whose value is invalidated by this row change
  ObSEArray<int64_t, 4> invalidated;
  if (OB_FAIL(get_batch(ai_ctdef.table_id_, batch))) {
  } else if (ObDmlEventType::DE_INSERTING == node.dml_event_) {
    if (OB_ISNULL(node.new_row_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(build_predicate(ai_ctdef, ctdef.new_row_, *node.new_row_,
                                       ai_ctdef.rowkey_idxs_, new_predicate))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_ctdef.column_ids_.count(); ++i) {
      ret = invalidated.push_back(i);
    }
  } else if (ObDmlEventType::DE_DELETING == node.dml_event_) {
    if (OB_ISNULL(node.old_row_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(build_predicate(ai_ctdef, ctdef.old_row_, *node.old_row_,
                                       ai_ctdef.rowkey_idxs_, old_predicate))) {
    } else {
      need_delete = true;
    }
  } else if (ObDmlEventType::DE_UPDATING == node.dml_event_) {
    const ObChunkDatumStore::StoredRow *old_row = NULL;
    const ObChunkDatumStore::StoredRow *new_row = NULL;
    const ExprFixedArray *new_exprs = NULL;
    const ObIArray<int64_t> *new_rowkey_idxs = NULL;
    const ObIArray<int64_t> *new_request_idxs = NULL;
    if (NULL != node.full_row_) {
      // regular update: old values first, then the assigned values
      old_row = node.full_row_;
      new_row = node.full_row_;
      new_exprs = &ctdef.full_row_;
      new_rowkey_idxs = &ai_ctdef.rowkey_full_idxs_;
      new_request_idxs = &ai_ctdef.request_full_idxs_;
    } else if (NULL != node.old_row_ && NULL != node.new_row_ && node.old_row_ != node.new_row_) {
      // primary key update written as delete and insert
      old_row = node.old_row_;
      new_row = node.new_row_;
      new_exprs = &ctdef.new_row_;
      new_rowkey_idxs = &ai_ctdef.rowkey_idxs_;
      new_request_idxs = &ai_ctdef.request_idxs_;
    } else {
      // the row is not changed
    }
    for (int64_t i = 0; OB_SUCC(ret) && NULL != old_row && i < ai_ctdef.column_ids_.count(); ++i) {
      const int64_t old_idx = ai_ctdef.request_idxs_.at(i);
      const int64_t new_idx = new_request_idxs->at(i);
      if (old_idx < 0 || new_idx < 0) {
      } else if (OB_UNLIKELY(old_idx >= old_row->cnt_ || new_idx >= new_row->cnt_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid request column position", K(ret), K(old_idx), K(new_idx));
      } else if (is_request_changed(old_row->cells()[old_idx], new_row->cells()[new_idx])) {
        ret = invalidated.push_back(i);
      }
    }
    if (OB_FAIL(ret) || invalidated.empty()) {
    } else if (OB_FAIL(build_predicate(ai_ctdef, ctdef.old_row_, *old_row,
                                       ai_ctdef.rowkey_idxs_, old_predicate))) {
    } else if (OB_FAIL(build_predicate(ai_ctdef, *new_exprs, *new_row,
                                       *new_rowkey_idxs, new_predicate))) {
    } else {
      need_delete = old_predicate.string() != new_predicate.string();
    }
  }
  if (OB_SUCC(ret) && need_delete && OB_FAIL(add_delete(*batch, old_predicate.string()))) {
  }
  if (OB_SUCC(ret) && !invalidated.empty()) {
    // the AI columns invalidated by one row change form one group with a shared request id
    const uint64_t request_id = ObExprUuidShort::generate_uuid_short();
    for (int64_t i = 0; OB_SUCC(ret) && i < invalidated.count(); ++i) {
      ret = add_task(*batch, ai_ctdef, invalidated.at(i), new_predicate.string(), request_id);
    }
  }
  if (OB_SUCC(ret)
      && (batch->deletes_.length() + batch->replaces_.length() > FLUSH_SIZE)
      && OB_FAIL(flush(*batch))) {
  }
  return ret;
}

int AITaskWriter::flush(AITaskBatch &batch)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  // task rows are written by nested SQL of this statement, marked the way foreign key
  // cascades mark theirs so that the nested statements join the current one
  ObDASCtx &das_ctx = op_.get_exec_ctx().get_das_ctx();
  const bool save_fk_cascading = das_ctx.is_fk_cascading_;
  if (batch.delete_cnt_ <= 0 && batch.replace_cnt_ <= 0) {
  } else if (FALSE_IT(das_ctx.is_fk_cascading_ = true)) {
  } else if (OB_FAIL(op_.begin_nested_session(false))) {
    LOG_WARN("failed to begin nested session", K(ret));
  } else {
    // deletes go first, an UPDATE may move a row onto a key another row has just left
    if (batch.delete_cnt_ > 0) {
      if (OB_FAIL(sql.assign_fmt("DELETE FROM oceanbase.__all_ai_column_task"
                                 " WHERE table_id = %lu AND rowkey_predicate IN (%.*s)",
                                 batch.table_id_,
                                 static_cast<int>(batch.deletes_.length()),
                                 batch.deletes_.ptr()))) {
      } else if (OB_FAIL(op_.execute_write(sql.ptr()))) {
        LOG_WARN("failed to delete ai column tasks", K(ret));
      }
    }
    if (OB_SUCC(ret) && batch.replace_cnt_ > 0) {
      if (OB_FAIL(sql.assign_fmt("REPLACE INTO oceanbase.__all_ai_column_task (table_id,"
                                 " rowkey_predicate, column_id, request_id, definition_version,"
                                 " state, retry_count, max_retries, eligible_ts, owner,"
                                 " lease_expire_ts, result, last_error) VALUES %.*s",
                                 static_cast<int>(batch.replaces_.length()),
                                 batch.replaces_.ptr()))) {
      } else if (OB_FAIL(op_.execute_write(sql.ptr()))) {
        LOG_WARN("failed to write ai column tasks", K(ret));
      } else {
        AIColumnTaskSignal::notify();
      }
    }
    int end_ret = op_.end_nested_session();
    if (OB_SUCCESS != end_ret) {
      LOG_WARN("failed to end nested session", K(end_ret));
      ret = OB_SUCCESS == ret ? end_ret : ret;
    }
  }
  das_ctx.is_fk_cascading_ = save_fk_cascading;
  batch.deletes_.reuse();
  batch.replaces_.reuse();
  batch.delete_cnt_ = 0;
  batch.replace_cnt_ = 0;
  return ret;
}

int AITaskWriter::flush_all()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < batches_.count(); ++i) {
    ret = flush(*batches_.at(i));
  }
  return ret;
}
} // namespace

int AIColumnDMLHandler::handle(ObTableModifyOp &op, ObDMLModifyRowsList &rows)
{
  int ret = OB_SUCCESS;
  bool has_ai_row = false;
  for (ObDMLModifyRowsList::iterator iter = rows.begin(); !has_ai_row && iter != rows.end(); ++iter) {
    has_ai_row = NULL != iter->dml_ctdef_ && iter->dml_ctdef_->ai_ctdef_.is_valid();
  }
  if (has_ai_row) {
    AITaskWriter writer(op);
    for (ObDMLModifyRowsList::iterator iter = rows.begin(); OB_SUCC(ret) && iter != rows.end(); ++iter) {
      if (NULL != iter->dml_ctdef_ && iter->dml_ctdef_->ai_ctdef_.is_valid()
          && OB_FAIL(writer.add_row(*iter))) {
        LOG_WARN("failed to add ai column task row", K(ret));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(writer.flush_all())) {
      LOG_WARN("failed to write ai column tasks", K(ret));
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
