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

#define USING_LOG_PREFIX COMMON

#include "sql/engine/expr/ob_expr_vec_vid.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/autoincrement/ob_i_tablet_autoincrement_service.h"
#include "share/rc/ob_server_runtime.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
ObExprVecVid::ObExprVecVid(ObIAllocator &allocator)
  : ObFuncExprOperator(allocator, T_FUN_SYS_VEC_VID, N_VEC_VID, ZERO_OR_ONE, VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
  need_charset_convert_ = false;
}

int ObExprVecVid::calc_result_typeN(ObExprResType &type,
                                       ObExprResType *types,
                                       int64_t param_num,
                                       ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  UNUSEDx(param_num, types);
  type.set_int();
  return ret;
}

int ObExprVecVid::calc_resultN(ObObj &result,
                                  const ObObj *objs_array,
                                  int64_t param_num,
                                  ObExprCtx &expr_ctx) const
{
  return OB_NOT_SUPPORTED;
}

int ObExprVecVid::cg_expr(
    ObExprCGCtx &cg_ctx,
    const ObRawExpr &raw_expr,
    ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(rt_expr.arg_cnt_ != 1 && rt_expr.arg_cnt_ != 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected param count", K(rt_expr.arg_cnt_), K(rt_expr.args_), K(rt_expr.type_));
  } else if (OB_UNLIKELY(rt_expr.arg_cnt_ == 1 && OB_ISNULL(rt_expr.args_))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error, rt_expr.args_ is nullptr", K(rt_expr.arg_cnt_), K(rt_expr.args_), K(rt_expr.type_));
  } else {
    rt_expr.eval_func_ = generate_vec_id;
  }
  return ret;
}

/*static*/ int ObExprVecVid::generate_vec_id(
    const ObExpr &raw_ctx,
    ObEvalCtx &eval_ctx,
    ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;
  if (raw_ctx.arg_cnt_ == 0) {
    expr_datum.set_null();
  } else if (OB_UNLIKELY(1 != raw_ctx.arg_cnt_) || OB_ISNULL(raw_ctx.args_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(raw_ctx), KP(raw_ctx.args_));
  } else {
    ObExpr *calc_part_id_expr = raw_ctx.args_[0];
    ObObjectID partition_id = OB_INVALID_ID;
    ObTabletID tablet_id;
    if (OB_FAIL(ObExprCalcPartitionBase::calc_part_and_tablet_id(calc_part_id_expr, eval_ctx, partition_id, tablet_id))) {
    } else {
      share::ObITabletAutoincrementService *auto_inc =
          ::oceanbase::share::server_service<::oceanbase::share::ObITabletAutoincrementService>();
      const ObSQLSessionInfo *session = eval_ctx.exec_ctx_.get_my_session();
      VidRangeCtx *range_ctx = nullptr;
      uint64_t seq_id = 0;
      if (OB_ISNULL(auto_inc)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tablet autoincrement service is unavailable", K(ret));
      } else if (OB_ISNULL(session) || !session->get_ddl_info().is_ddl()
                 || ObExpr::INVALID_EXP_CTX_ID == raw_ctx.expr_ctx_id_) {
        if (OB_FAIL(auto_inc->next_value(tablet_id, seq_id))) {
        }
      } else if (OB_ISNULL(range_ctx = static_cast<VidRangeCtx *>(
                     eval_ctx.exec_ctx_.get_expr_op_ctx(raw_ctx.expr_ctx_id_)))
                 && OB_FAIL(eval_ctx.exec_ctx_.create_expr_op_ctx(raw_ctx.expr_ctx_id_, range_ctx))) {
        LOG_WARN("failed to create vid range ctx", K(ret));
      } else if (OB_ISNULL(range_ctx)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("vid range ctx is null", K(ret));
      } else if (OB_FAIL(range_ctx->next_value(*auto_inc, tablet_id, seq_id))) {
      }
      if (OB_SUCC(ret)) {
        expr_datum.set_int(seq_id);
      }
    }
  }
  return ret;
}

// A DDL generates the vid of every row of a tablet. Taken from the shared tablet cache, each vid would lock it, and
// every 10000 vids would wait for the log of the next range. So the DDL reserves large ranges of its own, as the DDL
// writers do for LOB ids and hidden primary keys.
int ObExprVecVid::VidRangeCtx::next_value(share::ObITabletAutoincrementService &auto_inc,
                                          const ObTabletID &tablet_id,
                                          uint64_t &value)
{
  int ret = OB_SUCCESS;
  static const uint64_t VID_RANGE_SIZE = 500L * 10000L;
  if (range_.tablet_id_ != tablet_id || OB_EAGAIN == range_.next_value(value)) {
    if (OB_FAIL(auto_inc.next_interval(tablet_id, VID_RANGE_SIZE, range_))) {
      range_.reset();
      LOG_WARN("failed to reserve a vid range", K(ret), K(tablet_id));
    } else if (OB_FAIL(range_.next_value(value))) {
      LOG_WARN("reserved vid range is empty", K(ret), K_(range));
    }
  }
  return ret;
}


}  // namespace sql
}  // namespace oceanbase
