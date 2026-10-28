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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (ob_is_text_tc(types_stack[CONTENT_IDX].get_type())) {
      // TEXT/LONGTEXT content is read as lob, so it is not limited by the varchar length
      types_stack[CONTENT_IDX].set_calc_type(ObLongTextType);
    } else {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    }
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else if (ob_is_null(types_stack[DIM_IDX].get_type())) {
        // NULL dimension means the default dimension of the model
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::get_content(const ObExpr &expr,
                               ObEvalCtx &ctx,
                               ObIAllocator &allocator,
                               const ObDatum &datum,
                               ObString &content)
{
  INIT_SUCC(ret);
  const ObExpr *arg = expr.args_[CONTENT_IDX];
  if (OB_ISNULL(arg)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, datum,
                                                               arg->datum_meta_,
                                                               arg->obj_meta_.has_lob_header(),
                                                               content))) {
    LOG_WARN("failed to read content", K(ret));
  }
  return ret;
}

int ObExprAIEmbed::embed_contents(ObIAllocator &allocator,
                                  const ObString &model_id,
                                  const ObDatum *dim_datum,
                                  ObArray<ObString> &contents,
                                  ObArray<ObString> &results)
{
  INIT_SUCC(ret);
  ObAIFuncExprInfo *info = nullptr;
  ObJsonObject *config = nullptr;
  share::ObAiModelEndpointInfo resolved_endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  if (model_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < contents.count(); i++) {
    if (contents.at(i).empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_NOT_NULL(dim_datum) && !dim_datum->is_null()) {
    const int64_t dim = dim_datum->get_int();
    ObJsonInt *dim_json = nullptr;
    if (dim <= 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dim, dim_json))) {
    } else if (OB_FAIL(config->add("dimensions", dim_json))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_id, info))) {
  } else if (OB_ISNULL(info) || OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, allocator, resolved_endpoint))) {
  } else {
    ObAIFuncModel model(allocator, *info, resolved_endpoint);
    if (OB_FAIL(model.call_dense_embedding_batch(contents, config, results))) {
    } else if (OB_UNLIKELY(results.count() != contents.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("embedding result count mismatch", K(ret), K(results.count()), K(contents.count()));
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    // NULL in, NULL out, so that NULL rows do not fail the whole query
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObString model_id = arg_model_id->get_string();
    ObString content;
    ObArray<ObString> contents;
    ObArray<ObString> results;
    if (OB_FAIL(get_content(expr, ctx, temp_allocator, *arg_content, content))) {
    } else if (OB_FAIL(contents.push_back(content))) {
    } else if (OB_FAIL(embed_contents(temp_allocator, model_id, arg_dim, contents, results))) {
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, results.at(0)))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr,
                                       ObEvalCtx &ctx,
                                       const ObBitVector &skip,
                                       const int64_t batch_size)
{
  INIT_SUCC(ret);
  ObDatum *res = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; i++) {
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, skip, batch_size))) {
      LOG_WARN("failed to eval ai_embed args", K(ret), K(i));
    }
  }
  if (OB_SUCC(ret)) {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObEvalCtx::BatchInfoScopeGuard batch_info_guard(ctx);
    batch_info_guard.set_batch_size(batch_size);
    const bool has_dim = expr.arg_cnt_ > DIM_IDX;
    ObDatumVector model_datums = expr.locate_param_datumvector(ctx, MODEL_IDX);
    ObDatumVector content_datums = expr.locate_param_datumvector(ctx, CONTENT_IDX);
    ObDatumVector dim_datums;
    ObArray<int64_t> pending_rows;
    ObArray<bool> grouped;
    bool got_null = false;
    if (has_dim) {
      dim_datums = expr.locate_param_datumvector(ctx, DIM_IDX);
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; j++) {
      if (skip.at(j) || eval_flags.at(j)) {
      } else if (model_datums.at(j)->is_null() || content_datums.at(j)->is_null()) {
        // NULL rows are not sent, and keep their position in the batch
        res[j].set_null();
        eval_flags.set(j);
        got_null = true;
      } else if (OB_FAIL(pending_rows.push_back(j))) {
      } else if (OB_FAIL(grouped.push_back(false))) {
      }
    }
    // rows with the same model and dimension share requests, each request carries several rows
    ObArray<int64_t> group_rows;
    ObArray<ObString> contents;
    ObArray<ObString> results;
    for (int64_t p = 0; OB_SUCC(ret) && p < pending_rows.count(); p++) {
      if (grouped.at(p)) {
        continue;
      }
      const int64_t first_row = pending_rows.at(p);
      const ObString model_id = model_datums.at(first_row)->get_string();
      const ObDatum *dim_datum = has_dim ? dim_datums.at(first_row) : nullptr;
      group_rows.reuse();
      contents.reuse();
      results.reuse();
      for (int64_t q = p; OB_SUCC(ret) && q < pending_rows.count(); q++) {
        const int64_t row = pending_rows.at(q);
        const ObDatum *row_dim = has_dim ? dim_datums.at(row) : nullptr;
        ObString content;
        if (grouped.at(q)) {
        } else if (model_datums.at(row)->get_string() != model_id) {
        } else if (has_dim && !(row_dim->is_null() == dim_datum->is_null()
                                && (row_dim->is_null() || row_dim->get_int() == dim_datum->get_int()))) {
        } else if (OB_FAIL(get_content(expr, ctx, temp_allocator, *content_datums.at(row), content))) {
        } else if (OB_FAIL(contents.push_back(content))) {
        } else if (OB_FAIL(group_rows.push_back(row))) {
        } else {
          grouped.at(q) = true;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(embed_contents(temp_allocator, model_id, dim_datum, contents, results))) {
        LOG_WARN("failed to embed rows", K(ret), K(group_rows.count()));
      }
      // results are in the order of contents, so each vector goes back to its own row
      for (int64_t k = 0; OB_SUCC(ret) && k < group_rows.count(); k++) {
        const int64_t row = group_rows.at(k);
        batch_info_guard.set_batch_idx(row);
        if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res[row], results.at(k)))) {
          LOG_WARN("failed to set embedding result", K(ret), K(row));
        } else {
          eval_flags.set(row);
        }
      }
    }
    if (OB_SUCC(ret) && got_null) {
      expr.get_eval_info(ctx).notnull_ = false;
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
