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

#include "sql/engine/basic/ob_file_table_op.h"
#include "sql/engine/basic/ob_file_parquet_reader.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/ob_errno.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER_INHERIT(ObFileTableSpec, ObOpSpec, desc_, column_exprs_, column_idxs_);

ObFileTableOp::ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
  : ObOperator(exec_ctx, spec, input), row_alloc_("FileSqlRow"), open_alloc_("FileSqlOpen"),
    source_(NULL), values_(NULL), batch_datums_(NULL), iter_end_(false)
{}

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  const int64_t col_cnt = MY_SPEC.column_exprs_.count();
  iter_end_ = false;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is null", K(ret));
  } else if (OB_UNLIKELY(col_cnt != MY_SPEC.column_idxs_.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("column exprs do not match column indexes", K(ret), K(col_cnt));
  } else if (OB_FAIL(FileSqlUtil::check_enabled(*session))) {
    // the plan is never cached, but the variable may be switched off inside a procedure
  } else if (col_cnt > 0 && NULL == values_) {
    void *value_buf = ctx_.get_allocator().alloc(sizeof(FileValue) * col_cnt);
    void *datum_buf = ctx_.get_allocator().alloc(sizeof(ObDatum *) * col_cnt);
    if (OB_ISNULL(value_buf) || OB_ISNULL(datum_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate values", K(ret), K(col_cnt));
    } else {
      values_ = static_cast<FileValue *>(value_buf);
      for (int64_t i = 0; i < col_cnt; ++i) {
        new (&values_[i]) FileValue();
      }
      batch_datums_ = static_cast<ObDatum **>(datum_buf);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(create_source())) {
    LOG_WARN("failed to open file table", K(ret), K(MY_SPEC.desc_.path_));
  }
  return ret;
}

int ObFileTableOp::create_source()
{
  int ret = OB_SUCCESS;
  const FileTableDesc &desc = MY_SPEC.desc_;
  ObString secure_file_priv;
  ObString real_path;
  void *buf = NULL;
  ObIAllocator &alloc = ctx_.get_allocator();
  release_source();
  open_alloc_.reuse();
  if (OB_FAIL(FileSqlUtil::get_secure_file_priv(open_alloc_, secure_file_priv))) {
    LOG_WARN("failed to get secure_file_priv", K(ret));
  } else if (FileFormat::GLOB == desc.get_format()) {
    // glob checks every file it lists against secure_file_priv
  } else if (OB_FAIL(FileSqlUtil::resolve_input_path(desc.path_, secure_file_priv, open_alloc_,
                                                     real_path))) {
    // secure_file_priv is checked again: it may have changed since the query was resolved
  } else if (real_path != desc.real_path_) {
    ret = OB_TABLE_DEFINITION_CHANGED;
    FileSqlUtil::set_user_error(ret,
        "file sql: '%.*s' changed after its schema was inferred, please retry",
        desc.path_.length(), desc.path_.ptr());
  }
  if (OB_FAIL(ret)) {
  } else {
    switch (desc.get_format()) {
      case FileFormat::CSV:
        if (OB_NOT_NULL(buf = alloc.alloc(sizeof(CsvRowSource)))) {
          source_ = new (buf) CsvRowSource();
        }
        break;
      case FileFormat::JSON:
        if (OB_NOT_NULL(buf = alloc.alloc(sizeof(JsonRowSource)))) {
          source_ = new (buf) JsonRowSource();
        }
        break;
      case FileFormat::PARQUET:
        if (OB_NOT_NULL(buf = alloc.alloc(sizeof(ParquetRowSource)))) {
          source_ = new (buf) ParquetRowSource();
        }
        break;
      case FileFormat::GLOB:
        if (OB_NOT_NULL(buf = alloc.alloc(sizeof(GlobRowSource)))) {
          source_ = new (buf) GlobRowSource();
        }
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected file format", K(ret), K(desc.format_));
        break;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(source_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate file row source", K(ret));
    } else if (OB_FAIL(source_->open(desc, MY_SPEC.column_idxs_, secure_file_priv))) {
      LOG_WARN("failed to open file row source", K(ret));
    }
  }
  return ret;
}

void ObFileTableOp::release_source()
{
  if (NULL != source_) {
    source_->close();
    source_->~IFileRowSource();
    source_ = NULL;
  }
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  iter_end_ = false;
  if (OB_FAIL(ObOperator::inner_rescan())) {
    LOG_WARN("failed to rescan", K(ret));
  } else if (OB_ISNULL(source_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("file table is not opened", K(ret));
  } else if (OB_FAIL(source_->rewind())) {
    LOG_WARN("failed to rewind file table", K(ret));
  }
  return ret;
}

void ObFileTableOp::fill_datum(const int64_t col, const FileValue &value, ObDatum &datum) const
{
  const FileColumnDesc &column = MY_SPEC.desc_.columns_.at(MY_SPEC.column_idxs_.at(col));
  if (value.is_null_) {
    datum.set_null();
  } else {
    switch (column.get_type()) {
      case FileColumnType::BOOLEAN:
      case FileColumnType::BIGINT:
        datum.set_int(value.int_);
        break;
      case FileColumnType::DOUBLE:
        datum.set_double(value.double_);
        break;
      case FileColumnType::DATE:
        datum.set_date(static_cast<int32_t>(value.int_));
        break;
      case FileColumnType::DATETIME:
        datum.set_datetime(value.int_);
        break;
      case FileColumnType::VARCHAR:
        datum.set_string(value.str_.ptr(), value.str_.length());
        break;
      default:
        datum.set_null();
        break;
    }
  }
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  row_alloc_.reuse();
  if (OB_ISNULL(source_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("file table is not opened", K(ret));
  } else if (iter_end_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(try_check_status())) {
    LOG_WARN("failed to check status", K(ret));
  } else if (OB_FAIL(source_->next_row(row_alloc_, values_))) {
    if (OB_ITER_END == ret) {
      iter_end_ = true;
    } else {
      LOG_WARN("failed to read file row", K(ret));
    }
  } else {
    for (int64_t i = 0; i < MY_SPEC.column_exprs_.count(); ++i) {
      const ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      fill_datum(i, values_[i], expr->locate_datum_for_write(eval_ctx_));
      expr->set_evaluated_projected(eval_ctx_);
    }
  }
  return ret;
}

int ObFileTableOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  const int64_t batch_size = std::min(max_row_cnt, MY_SPEC.max_batch_size_);
  const int64_t col_cnt = MY_SPEC.column_exprs_.count();
  int64_t size = 0;
  clear_evaluated_flag();
  row_alloc_.reuse();
  if (OB_ISNULL(source_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("file table is not opened", K(ret));
  } else if (iter_end_) {
  } else if (OB_FAIL(check_status())) {
    LOG_WARN("failed to check status", K(ret));
  } else {
    for (int64_t i = 0; i < col_cnt; ++i) {
      batch_datums_[i] = MY_SPEC.column_exprs_.at(i)->locate_datums_for_update(eval_ctx_,
                                                                                batch_size);
    }
    while (OB_SUCC(ret) && size < batch_size) {
      if (OB_FAIL(source_->next_row(row_alloc_, values_))) {
        if (OB_ITER_END == ret) {
          iter_end_ = true;
          ret = OB_SUCCESS;
          break;
        } else {
          LOG_WARN("failed to read file row", K(ret));
        }
      } else {
        for (int64_t i = 0; i < col_cnt; ++i) {
          fill_datum(i, values_[i], batch_datums_[i][size]);
        }
        ++size;
      }
    }
  }
  if (OB_SUCC(ret)) {
    brs_.size_ = size;
    brs_.end_ = iter_end_ && 0 == size;
  }
  return ret;
}

int ObFileTableOp::inner_close()
{
  release_source();
  row_alloc_.reuse();
  open_alloc_.reuse();
  return OB_SUCCESS;
}

void ObFileTableOp::destroy()
{
  release_source();
  row_alloc_.reset();
  open_alloc_.reset();
  ObOperator::destroy();
}

} // namespace sql
} // namespace oceanbase
