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

#include "sql/optimizer/ob_logical_operator.h"

namespace oceanbase
{
namespace sql
{
struct FileTableDesc;

// FILE TABLE SCAN: leaf operator reading a read_csv / read_json / read_parquet / glob table.
class ObLogFileTable : public ObLogicalOperator
{
public:
  ObLogFileTable(ObLogPlan &plan)
    : ObLogicalOperator(plan), table_id_(common::OB_INVALID_ID), table_name_(),
      file_table_def_(NULL), file_rows_(0), est_rows_(0), access_exprs_()
  {}
  virtual ~ObLogFileTable() {}
  virtual uint64_t hash(uint64_t seed) const override;
  virtual int est_cost() override;
  virtual int do_re_est_cost(EstimateCostInfo &param, double &card, double &op_cost,
                             double &cost) override;
  virtual int get_op_exprs(ObIArray<ObRawExpr*> &all_exprs) override;
  virtual int is_my_fixed_expr(const ObRawExpr *expr, bool &is_fixed) override;
  virtual int allocate_expr_post(ObAllocExprContext &ctx) override;
  virtual int get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item) override;
  // Columns of this table referenced by the statement; the other columns are never read.
  int generate_access_exprs();
  common::ObIArray<ObRawExpr*> &get_access_exprs() { return access_exprs_; }
  const common::ObIArray<ObRawExpr*> &get_access_exprs() const { return access_exprs_; }
  void set_table_id(const uint64_t table_id) { table_id_ = table_id; }
  uint64_t get_table_id() const { return table_id_; }
  void set_table_name(const common::ObString &table_name) { table_name_ = table_name; }
  const common::ObString &get_table_name() const { return table_name_; }
  void set_file_table_def(const FileTableDesc *def) { file_table_def_ = def; }
  const FileTableDesc *get_file_table_def() const { return file_table_def_; }
  void set_rows(const double file_rows, const double est_rows)
  {
    file_rows_ = file_rows;
    est_rows_ = est_rows;
  }
private:
  int allocate_dummy_output();
private:
  uint64_t table_id_;
  common::ObString table_name_;
  const FileTableDesc *file_table_def_;
  double file_rows_;   // rows of the file found by inference
  double est_rows_;    // rows after the filters
  common::ObSEArray<ObRawExpr*, 8, common::ModulePageAllocator, true> access_exprs_;
  DISALLOW_COPY_AND_ASSIGN(ObLogFileTable);
};

} // namespace sql
} // namespace oceanbase
