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

// Scan of a file table: read_csv / read_json / read_parquet / glob in the FROM clause.
// Rows are streamed from the file in batches; only the referenced columns are produced.

#pragma once

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

class ObFileTableSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), desc_(alloc), column_exprs_(alloc), column_idxs_(alloc) {}
  INHERIT_TO_STRING_KV("op_spec", ObOpSpec, K_(desc), K_(column_idxs));
  FileTableDesc desc_;
  // produced column exprs and the index of each one in desc_.columns_
  ExprFixedArray column_exprs_;
  common::ObFixedArray<int64_t, common::ObIAllocator> column_idxs_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObFileTableSpec);
};

class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input);
  virtual ~ObFileTableOp() { release_source(); }
  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_get_next_batch(const int64_t max_row_cnt) override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int create_source();
  void release_source();
  void fill_datum(const int64_t col, const FileValue &value, ObDatum &datum) const;
private:
  common::ObArenaAllocator row_alloc_;      // VARCHAR values of the current row / batch
  common::ObArenaAllocator open_alloc_;     // secure_file_priv and the checked path
  IFileRowSource *source_;
  FileValue *values_;                       // one value per projected column
  ObDatum **batch_datums_;                  // datums of each projected column in a batch
  bool iter_end_;
  DISALLOW_COPY_AND_ASSIGN(ObFileTableOp);
};

} // namespace sql
} // namespace oceanbase
