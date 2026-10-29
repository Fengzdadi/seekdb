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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"
#include "share/datum/ob_datum_cmp_func_def.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (param.base_param_->need_project_relevance()
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else if (OB_FAIL(init_linear_merge())) {
  } else if (OB_FAIL(init_batch_filter())) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
  }
  return ret;
}

int ObTextDaaTIter::init_linear_merge()
{
  int ret = OB_SUCCESS;
  const int64_t dim_cnt = dim_iters_->count();
  use_linear_merge_ = false;
  if (dim_cnt > 0 && dim_cnt <= LINEAR_MERGE_MAX_DIM_CNT) {
    const sql::ObDatumMeta &id_meta = iter_param_->id_proj_expr_->datum_meta_;
    common::ObDatumBasicFuncs *basic_funcs = ObDatumFuncs::get_basic_func(id_meta.type_, id_meta.cs_type_);
    void *iters_buf = iter_allocator_->alloc(sizeof(ObTextRetrievalDaaTTokenIter *) * dim_cnt);
    void *relevances_buf = iter_allocator_->alloc(sizeof(double) * dim_cnt);
    void *active_buf = iter_allocator_->alloc(sizeof(int64_t) * dim_cnt);
    if (OB_ISNULL(basic_funcs) || OB_ISNULL(basic_funcs->null_first_cmp_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(iters_buf) || OB_ISNULL(relevances_buf) || OB_ISNULL(active_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      // same comparison as the merge heap, see ObSRMergeCmp::init
      id_cmp_func_ = basic_funcs->null_first_cmp_;
      is_binary_id_ = (ObVarcharType == id_meta.type_ || ObCharType == id_meta.type_)
                      && CS_TYPE_BINARY == id_meta.cs_type_;
      token_iters_ = static_cast<ObTextRetrievalDaaTTokenIter **>(iters_buf);
      dim_relevances_ = static_cast<double *>(relevances_buf);
      active_dims_ = static_cast<int64_t *>(active_buf);
      for (int64_t i = 0; i < dim_cnt; ++i) {
        token_iters_[i] = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i));
        dim_relevances_[i] = 0.0;
      }
      use_linear_merge_ = true;
      reuse_linear_merge();
    }
  }
  return ret;
}

void ObTextDaaTIter::reuse_linear_merge()
{
  if (use_linear_merge_) {
    active_dim_cnt_ = dim_iters_->count();
    for (int64_t i = 0; i < active_dim_cnt_; ++i) {
      active_dims_[i] = i;
    }
  }
}

int ObTextDaaTIter::fill_merge_heap()
{
  int ret = OB_SUCCESS;
  if (!use_linear_merge_) {
    ret = ObSRDaaTIterImpl::fill_merge_heap();
  } else {
    // relevance of a dim is computed as in ObSRDaaTIterImpl::fill_merge_heap
    double relevance = 0.0;
    const int64_t *round_iter_idxes = next_round_iter_idxes_.get_data();
    const ObDatum **iter_ids = iter_domain_ids_.get_data();
    for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
      const int64_t iter_idx = round_iter_idxes[i];
      ObTextRetrievalDaaTTokenIter *token_iter = token_iters_[iter_idx];
      if (OB_FAIL(token_iter->next_row())) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
          LOG_WARN("fail to get next row from dim iter", K(ret), K(iter_idx));
        } else {
          ret = OB_SUCCESS;
          int64_t pos = 0;
          while (pos < active_dim_cnt_ && active_dims_[pos] != iter_idx) {
            ++pos;
          }
          if (pos < active_dim_cnt_) {
            --active_dim_cnt_;
            for (; pos < active_dim_cnt_; ++pos) {
              active_dims_[pos] = active_dims_[pos + 1];
            }
          }
        }
      } else {
        if (token_iter->has_relevance()) {
          relevance = token_iter->curr_relevance();
        }
        if (OB_NOT_NULL(iter_param_->dim_weights_)) {
          relevance = relevance * iter_param_->field_boost_ * iter_param_->dim_weights_->at(iter_idx);
        }
        dim_relevances_[iter_idx] = relevance;
        iter_ids[iter_idx] = &token_iter->curr_id();
      }
    }
    if (OB_FAIL(ret)) {
    } else if (0 == active_dim_cnt_) {
      ret = OB_ITER_END;
    } else {
      next_round_cnt_ = 0;
    }
  }
  return ret;
}

// Same result as id_cmp_func_. For binary strings it is ObNullSafeDatumStrCmp, whose result does not
// depend on trailing spaces, and its comparison of two valid strings is inlined here.
OB_INLINE int ObTextDaaTIter::cmp_id(const ObDatum &l, const ObDatum &r, int &cmp_ret) const
{
  int ret = OB_SUCCESS;
  if (!is_binary_id_ || l.is_null() || r.is_null() || nullptr == l.ptr_ || nullptr == r.ptr_) {
    ret = id_cmp_func_(l, r, cmp_ret, nullptr);
  } else {
    ret = common::datum_cmp::ObDatumStrCmpImpl::cmp(l, r, cmp_ret, CS_TYPE_BINARY, false);
  }
  return ret;
}

int ObTextDaaTIter::collect_dims_by_id(const ObDatum *&id_datum, double &relevance, bool &got_valid_id)
{
  int ret = OB_SUCCESS;
  if (!use_linear_merge_) {
    ret = ObSRDaaTIterImpl::collect_dims_by_id(id_datum, relevance, got_valid_id);
  } else if (OB_UNLIKELY(active_dim_cnt_ <= 0)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObDatum **iter_ids = iter_domain_ids_.get_data();
    int64_t *round_iter_idxes = next_round_iter_idxes_.get_data();
    relevance = 0.0;
    got_valid_id = false;
    // The dims with the smallest doc id are collected in one pass, in ascending dim order: a
    // smaller doc id restarts the collected dims, and the first collected dim has the smallest id.
    const int64_t first_idx = next_round_cnt_;
    int64_t end_idx = first_idx + 1;
    round_iter_idxes[first_idx] = active_dims_[0];
    const ObDatum *min_id = iter_ids[active_dims_[0]];
    for (int64_t pos = 1; OB_SUCC(ret) && pos < active_dim_cnt_; ++pos) {
      const int64_t dim_idx = active_dims_[pos];
      const ObDatum *id = iter_ids[dim_idx];
      int cmp_ret = 0;
      if (OB_FAIL(cmp_id(*id, *min_id, cmp_ret))) {
        LOG_WARN("fail to compare doc id", K(ret));
      } else if (cmp_ret < 0) {
        min_id = id;
        round_iter_idxes[first_idx] = dim_idx;
        end_idx = first_idx + 1;
      } else if (0 == cmp_ret) {
        round_iter_idxes[end_idx++] = dim_idx;
      }
    }
    for (int64_t i = first_idx; OB_SUCC(ret) && i < end_idx; ++i) {
      const int64_t dim_idx = round_iter_idxes[i];
      if (OB_FAIL(relevance_collector_->collect_one_dim(dim_idx, dim_relevances_[dim_idx]))) {
        LOG_WARN("fail to collect dim", K(ret), K(dim_idx));
      }
    }
    if (OB_SUCC(ret)) {
      next_round_cnt_ = end_idx;
      id_datum = iter_ids[round_iter_idxes[end_idx - 1]];
      if (OB_ISNULL(id_datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(relevance_collector_->get_result(relevance, got_valid_id))) {
      } else if (got_valid_id && OB_FAIL(process_collected_row(*id_datum, relevance))) {
      }
    }
  }
  return ret;
}

int ObTextDaaTIter::init_batch_filter()
{
  int ret = OB_SUCCESS;
  sql::ObExpr *filter_expr = iter_param_->filter_expr_;
  sql::ObExpr *relevance_proj_expr = iter_param_->relevance_proj_expr_;
  sql::ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
  use_batch_filter_ = false;
  if (OB_ISNULL(filter_expr) || OB_ISNULL(relevance_proj_expr) || OB_ISNULL(eval_ctx)
      || !eval_ctx->is_vectorized() || !filter_expr->is_batch_result()
      || OB_ISNULL(filter_expr->eval_batch_func_) || !relevance_proj_expr->is_batch_result()) {
    // the filter is evaluated row by row
  } else {
    // Only the filter itself is re-evaluated for each batch, so it must directly use the score
    // and constants, as filter_on_demand() also assumes.
    bool direct_filter = true;
    for (int64_t i = 0; direct_filter && i < filter_expr->arg_cnt_; ++i) {
      const sql::ObExpr *arg = filter_expr->args_[i];
      direct_filter = relevance_proj_expr == arg || (OB_NOT_NULL(arg) && !arg->is_batch_result());
    }
    if (direct_filter) {
      const int64_t skip_size = sql::ObBitVector::memory_size(eval_ctx->max_batch_size_);
      void *skip_buf = iter_allocator_->alloc(skip_size);
      if (OB_ISNULL(skip_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        filter_skip_ = sql::to_bit_vector(skip_buf);
        filter_skip_->reset(eval_ctx->max_batch_size_);
        use_batch_filter_ = true;
      }
    }
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  batch_capacity_ = capacity;
  return ObSRDaaTIterImpl::get_next_rows(capacity, count);
}

int ObTextDaaTIter::do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  // the eval ctx is not null if use_batch_filter_ is set
  const int64_t end_idx = !use_batch_filter_ ? 0 : MIN(MIN(batch_capacity_, iter_param_->max_batch_size_),
                                                       iter_param_->eval_ctx_->max_batch_size_);
  if (!iter_param_->need_filter() && !iter_param_->limit_param_->is_valid()) {
    ret = merge_rows(count);
  } else if (!use_batch_filter_ || iter_param_->limit_param_->is_valid() || count >= end_idx) {
    ret = ObSRDaaTIterImpl::do_one_merge_round(count);
  } else {
    // Without a limit or an offset every row that passes the filter is output, in merge order,
    // so filtering the rows of the rest of the batch together outputs the same rows.
    int64_t buffered_cnt = count;
    while (OB_SUCC(ret) && buffered_cnt < end_idx) {
      const ObDatum *id_datum = nullptr;
      double relevance = 0.0;
      bool got_valid_id = false;
      if (OB_FAIL(fill_merge_heap())) {
      } else if (OB_FAIL(collect_dims_by_id(id_datum, relevance, got_valid_id))) {
      } else if (!got_valid_id) {
      } else if (OB_UNLIKELY(buffered_cnt >= buffered_domain_ids_.count()
                             || buffered_cnt >= buffered_relevances_.count())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(buffered_domain_ids_[buffered_cnt].from_datum(*id_datum))) {
      } else {
        buffered_relevances_[buffered_cnt] = relevance;
        ++buffered_cnt;
      }
    }
    if ((OB_SUCC(ret) || OB_ITER_END == ret) && buffered_cnt > count) {
      int tmp_ret = filter_buffered_rows(count, buffered_cnt, count);
      if (OB_SUCCESS != tmp_ret) {
        ret = tmp_ret;
      }
    }
  }
  return ret;
}

// Without a filter or a limit every valid row is output, as filter_on_demand() and cache_result()
// do, so rows are merged up to the capacity in one loop.
int ObTextDaaTIter::merge_rows(int64_t &count)
{
  int ret = OB_SUCCESS;
  const int64_t end_idx = MIN(batch_capacity_, iter_param_->max_batch_size_);
  ObDocIdExt *buffered_ids = buffered_domain_ids_.get_data();
  double *buffered_relevances = buffered_relevances_.get_data();
  while (OB_SUCC(ret) && count < end_idx) {
    const ObDatum *id_datum = nullptr;
    double relevance = 0.0;
    bool got_valid_id = false;
    if (OB_FAIL(fill_merge_heap())) {
    } else if (OB_FAIL(collect_dims_by_id(id_datum, relevance, got_valid_id))) {
    } else if (!got_valid_id) {
    } else if (OB_UNLIKELY(count >= buffered_domain_ids_.count() || count >= buffered_relevances_.count())) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ++input_row_cnt_;
      buffered_ids[count].from_datum(*id_datum);
      buffered_relevances[count] = relevance;
      ++count;
      ++output_row_cnt_;
    }
  }
  return ret;
}

// Evaluates the filter on buffered rows [begin, end) and moves the rows that pass it to
// [begin, new_end), keeping their order.
int ObTextDaaTIter::filter_buffered_rows(const int64_t begin, const int64_t end, int64_t &new_end)
{
  int ret = OB_SUCCESS;
  sql::ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
  sql::ObExpr *filter_expr = iter_param_->filter_expr_;
  sql::ObExpr *relevance_proj_expr = iter_param_->relevance_proj_expr_;
  sql::ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
  guard.set_batch_size(end);
  ObDatum *relevance_datums = relevance_proj_expr->locate_batch_datums(*eval_ctx);
  sql::ObBitVector &relevance_evaluated = relevance_proj_expr->get_evaluated_flags(*eval_ctx);
  for (int64_t i = begin; i < end; ++i) {
    relevance_proj_expr->reset_ptr_in_datum(*eval_ctx, i);
    relevance_datums[i].set_double(buffered_relevances_[i]);
    relevance_evaluated.set(i);
  }
  relevance_proj_expr->set_evaluated_projected(*eval_ctx);
  // rows before begin are already output
  filter_skip_->reset(end);
  if (begin > 0) {
    filter_skip_->set_all(begin);
  }
  filter_expr->get_eval_info(*eval_ctx).clear_evaluated_flag();
  if (OB_FAIL(filter_expr->eval_batch(*eval_ctx, *filter_skip_, end))) {
  } else {
    const ObDatum *filter_res = filter_expr->locate_batch_datums(*eval_ctx);
    int64_t output_idx = begin;
    for (int64_t i = begin; i < end; ++i) {
      if (!(filter_res[i].is_null() || 0 == filter_res[i].get_int())) {
        if (output_idx != i) {
          buffered_domain_ids_[output_idx] = buffered_domain_ids_[i];
          buffered_relevances_[output_idx] = buffered_relevances_[i];
        }
        ++output_idx;
      }
    }
    input_row_cnt_ += output_idx - begin;
    output_row_cnt_ += output_idx - begin;
    new_end = output_idx;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  use_linear_merge_ = false;
  is_binary_id_ = false;
  id_cmp_func_ = nullptr;
  token_iters_ = nullptr;
  dim_relevances_ = nullptr;
  active_dims_ = nullptr;
  active_dim_cnt_ = 0;
  use_batch_filter_ = false;
  batch_capacity_ = 0;
  filter_skip_ = nullptr;
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
  reuse_linear_merge();
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
