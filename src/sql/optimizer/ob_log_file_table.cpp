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

#define USING_LOG_PREFIX SQL_OPT

#include "sql/optimizer/ob_log_file_table.h"
#include "sql/optimizer/ob_log_plan.h"
#include "sql/optimizer/ob_join_order.h"
#include "sql/optimizer/ob_opt_est_cost.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

uint64_t ObLogFileTable::hash(uint64_t seed) const
{
  seed = do_hash(table_name_, seed);
  seed = ObLogicalOperator::hash(seed);
  return seed;
}

int ObLogFileTable::est_cost()
{
  int ret = OB_SUCCESS;
  double card = 0.0;
  double op_cost = 0.0;
  double cost = 0.0;
  EstimateCostInfo param;
  param.need_parallel_ = get_parallel();
  if (OB_FAIL(do_re_est_cost(param, card, op_cost, cost))) {
    LOG_WARN("failed to estimate cost", K(ret));
  } else {
    set_card(card);
    set_op_cost(op_cost);
    set_cost(cost);
  }
  return ret;
}

int ObLogFileTable::do_re_est_cost(EstimateCostInfo &param, double &card, double &op_cost,
                                   double &cost)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("plan is null", K(ret));
  } else {
    ObOptimizerContext &opt_ctx = get_plan()->get_optimizer_context();
    double read_rows = file_rows_;
    card = est_rows_;
    if (param.need_row_count_ >= 0 && param.need_row_count_ < card) {
      read_rows = read_rows * param.need_row_count_ / card;
      card = param.need_row_count_;
    }
    op_cost = ObOptEstCost::cost_values_table(read_rows, filter_exprs_, opt_ctx);
    cost = op_cost;
  }
  return ret;
}

int ObLogFileTable::generate_access_exprs()
{
  int ret = OB_SUCCESS;
  const ObDMLStmt *stmt = get_stmt();
  access_exprs_.reuse();
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("stmt is null", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < stmt->get_column_size(); ++i) {
    const ColumnItem *col_item = stmt->get_column_item(i);
    if (OB_ISNULL(col_item) || OB_ISNULL(col_item->expr_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column item is null", K(ret), K(i));
    } else if (col_item->table_id_ == table_id_
               && col_item->expr_->is_explicited_reference()
               && OB_FAIL(access_exprs_.push_back(col_item->expr_))) {
      LOG_WARN("failed to push back column expr", K(ret));
    }
  }
  return ret;
}

int ObLogFileTable::get_op_exprs(ObIArray<ObRawExpr*> &all_exprs)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(generate_access_exprs())) {
    LOG_WARN("failed to generate access exprs", K(ret));
  } else if (OB_FAIL(append(all_exprs, access_exprs_))) {
    LOG_WARN("failed to append access exprs", K(ret));
  } else if (OB_FAIL(ObLogicalOperator::get_op_exprs(all_exprs))) {
    LOG_WARN("failed to get op exprs", K(ret));
  }
  return ret;
}

int ObLogFileTable::is_my_fixed_expr(const ObRawExpr *expr, bool &is_fixed)
{
  is_fixed = ObOptimizerUtil::find_item(access_exprs_, expr);
  return OB_SUCCESS;
}

int ObLogFileTable::allocate_expr_post(ObAllocExprContext &ctx)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < access_exprs_.count(); ++i) {
    ObRawExpr *col = access_exprs_.at(i);
    if (OB_FAIL(mark_expr_produced(col, branch_id_, id_, ctx))) {
      LOG_WARN("failed to mark expr produced", K(ret));
    } else if (!is_plan_root() && OB_FAIL(add_var_to_array_no_dup(output_exprs_, col))) {
      LOG_WARN("failed to add output expr", K(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObLogicalOperator::allocate_expr_post(ctx))) {
    LOG_WARN("failed to allocate expr post", K(ret));
  } else if (get_output_exprs().empty() && OB_FAIL(allocate_dummy_output())) {
    LOG_WARN("failed to allocate dummy output", K(ret));
  }
  return ret;
}

// count(*) reads no column; the scan still needs an output to carry the rows.
int ObLogFileTable::allocate_dummy_output()
{
  int ret = OB_SUCCESS;
  ObConstRawExpr *dummy_expr = NULL;
  if (OB_ISNULL(get_plan())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("plan is null", K(ret));
  } else if (OB_FAIL(ObRawExprUtils::build_const_int_expr(
                         get_plan()->get_optimizer_context().get_expr_factory(), ObIntType, 1,
                         dummy_expr))) {
    LOG_WARN("failed to build const int expr", K(ret));
  } else if (OB_ISNULL(dummy_expr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dummy expr is null", K(ret));
  } else if (OB_FAIL(dummy_expr->extract_info())) {
    LOG_WARN("failed to extract info", K(ret));
  } else if (OB_FAIL(output_exprs_.push_back(dummy_expr))) {
    LOG_WARN("failed to push back dummy expr", K(ret));
  } else if (OB_FAIL(get_plan()->get_optimizer_context().get_all_exprs().append(dummy_expr))) {
    LOG_WARN("failed to append dummy expr", K(ret));
  }
  return ret;
}

int ObLogFileTable::get_plan_item_info(PlanText &plan_text, ObSqlPlanItem &plan_item)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObLogicalOperator::get_plan_item_info(plan_text, plan_item))) {
    LOG_WARN("failed to get plan item info", K(ret));
  } else {
    BEGIN_BUF_PRINT;
    const ObIArray<ObRawExpr*> &access = access_exprs_;
    EXPLAIN_PRINT_EXPRS(access, type);
    if (OB_SUCC(ret) && NULL != file_table_def_) {
      const ObString &func = file_table_def_->func_name_;
      const ObString &path = file_table_def_->path_;
      if (OB_FAIL(BUF_PRINTF(", %.*s('%.*s')", func.length(), func.ptr(), path.length(),
                             path.ptr()))) {
        LOG_WARN("failed to print file", K(ret));
      }
    }
    END_BUF_PRINT(plan_item.special_predicates_, plan_item.special_predicates_len_);
    if (OB_SUCC(ret)) {
      const ObString &name = get_table_name();
      BUF_PRINT_OB_STR(name.ptr(), name.length(), plan_item.object_alias_,
                       plan_item.object_alias_len_);
      BUF_PRINT_STR("FILE TABLE", plan_item.object_type_, plan_item.object_type_len_);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
