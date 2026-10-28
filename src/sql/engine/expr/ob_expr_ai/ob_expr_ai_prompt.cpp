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
#include "ob_expr_ai_prompt.h"
#include <regex>
#include "ob_ai_func.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"


using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIPrompt::ObExprAIPrompt(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_PROMPT, 
                    N_AI_PROMPT, 
                    MORE_THAN_ZERO,
                    VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIPrompt::~ObExprAIPrompt() 
{
}

int ObExprAIPrompt::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const 
{
  int ret = OB_SUCCESS;
  UNUSED(type_ctx);
  for (int64_t i = 0; i < param_num && OB_SUCC(ret); i++) {
    const ObObjType in_type = types_stack[i].get_type();
    if (ob_is_string_tc(in_type)) {
      types_stack[i].set_calc_type(ObVarcharType);
      types_stack[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (ob_is_text_tc(in_type)) {
      // TEXT/LONGTEXT/BLOB template and args are read as lob
      types_stack[i].set_calc_type(ObLongTextType);
      types_stack[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (i > 0 && ob_is_json(in_type)) {
      // json args are kept as json values
    } else {
      ret = OB_ERR_INVALID_TYPE_FOR_OP;
    }
  }

  if (OB_SUCC(ret)) {
    type.set_json();
    type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
  }
  return ret;
}

int ObExprAIPrompt::get_string_arg(const ObExpr &arg, ObEvalCtx &ctx, ObIAllocator &allocator,
                                   const ObDatum &datum, ObString &str)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, allocator, datum, arg.datum_meta_,
                                                        arg.obj_meta_.has_lob_header(), str))) {
    LOG_WARN("failed to read string data", K(ret));
  }
  return ret;
}

int ObExprAIPrompt::eval_ai_prompt(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *template_datum = NULL;
  ObExpr *arg0 = expr.args_[0];
  ObString template_str;
  ObJsonArray *args_array = NULL;
  ObJsonObject *prompt_object = NULL;
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor tmp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_PROMPT));

  if (OB_FAIL(tmp_allocator.eval_arg(arg0, ctx, template_datum))) {
  } else if (template_datum->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(get_string_arg(*arg0, ctx, tmp_allocator, *template_datum, template_str))) {
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(tmp_allocator, args_array))) {
  } else {
    for (int64_t i = 1; i < expr.arg_cnt_ && OB_SUCC(ret); i++) {
      ObDatum *datum = NULL;
      ObExpr *arg = expr.args_[i];
      ObString arg_str;
      ObJsonNode *arg_node = NULL;
      if (OB_FAIL(tmp_allocator.eval_arg(arg, ctx, datum))) {
      } else if (ob_is_json(arg->datum_meta_.type_)) {
        ObIJsonBase *j_base = NULL;
        ObIJsonBase *j_tree = NULL;
        bool is_null = false;
        if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, tmp_allocator, static_cast<uint16_t>(i), j_base, is_null))) {
          LOG_WARN("failed to get json arg", K(ret), K(i));
        } else if (is_null) {
          arg_node = OB_NEWx(ObJsonNull, &tmp_allocator);
        } else if (OB_ISNULL(j_base)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (j_base->is_tree()) {
          arg_node = static_cast<ObJsonNode *>(j_base);
        } else if (OB_FAIL(ObJsonBaseFactory::transform(&tmp_allocator, j_base, ObJsonInType::JSON_TREE, j_tree))) {
          LOG_WARN("failed to transform json arg to tree", K(ret), K(i));
        } else {
          arg_node = static_cast<ObJsonNode *>(j_tree);
        }
      } else if (datum->is_null()) {
        // like json_array(), a NULL arg is kept as json null
        arg_node = OB_NEWx(ObJsonNull, &tmp_allocator);
      } else if (OB_FAIL(get_string_arg(*arg, ctx, tmp_allocator, *datum, arg_str))) {
      } else {
        arg_node = OB_NEWx(ObJsonString, &tmp_allocator, arg_str);
      }
      if (OB_FAIL(ret)) {
      } else if (OB_ISNULL(arg_node)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(args_array->append(arg_node))) {
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(tmp_allocator, template_str, args_array, prompt_object))) {
  } else {
    ObString raw_str;
    if (OB_FAIL(prompt_object->get_raw_binary(raw_str, &tmp_allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}



int ObExprAIPrompt::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprAIPrompt::eval_ai_prompt;
  return ret;
}

} // namespace sql
} // namespace oceanbase
