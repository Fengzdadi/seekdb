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

#ifndef OB_TEXT_DAAT_ITER_H_
#define OB_TEXT_DAAT_ITER_H_

#include "query/ob_sql_define.h"
#include "ob_inv_idx_param_estimator.h"
#include "ob_sparse_bmw_iter.h"
#include "ob_sparse_daat_iter.h"
#include "ob_text_retrieval_token_iter.h"

namespace oceanbase
{
namespace sql
{
  class ObDASScanIter;
}
namespace storage
{

struct ObTextDaaTParam
{
  ObTextDaaTParam()
    : dim_iters_(nullptr), base_param_(nullptr), allocator_(nullptr), relevance_collector_(nullptr),
      bm25_param_est_ctx_(), mode_flag_(sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE),
      function_lookup_mode_(false) {}
  ~ObTextDaaTParam() {}
  TO_STRING_KV(K_(base_param), KP_(dim_iters), K_(bm25_param_est_ctx),  K_(mode_flag), K_(function_lookup_mode));
  ObIArray<ObISRDaaTDimIter *> *dim_iters_;
  ObSparseRetrievalMergeParam *base_param_;
  common::ObArenaAllocator *allocator_;
  ObSRDaaTRelevanceCollector *relevance_collector_;
  ObBM25ParamEstCtx bm25_param_est_ctx_;
  sql::ObMatchAgainstMode mode_flag_;
  bool function_lookup_mode_;
};

class ObTextDaaTIter final : public ObSRDaaTIterImpl
{
public:
  ObTextDaaTIter() : ObSRDaaTIterImpl(),
      bm25_param_estimator_(),
      mode_flag_(sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE),
      function_lookup_mode_(false),
      use_linear_merge_(false),
      id_cmp_func_(nullptr),
      token_iters_(nullptr),
      dim_relevances_(nullptr),
      active_dims_(nullptr),
      active_dim_cnt_(0),
      use_batch_filter_(false),
      batch_capacity_(0),
      filter_skip_(nullptr) {}
  virtual ~ObTextDaaTIter() { reset(); }

  int init(const ObTextDaaTParam &param);
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
protected:
  virtual int pre_process() override;
  virtual int do_one_merge_round(int64_t &count) override;
  virtual int fill_merge_heap() override;
  virtual int collect_dims_by_id(const ObDatum *&id_datum, double &relevance, bool &got_valid_id) override;
private:
  int init_linear_merge();
  void reuse_linear_merge();
  int init_batch_filter();
  int filter_buffered_rows(const int64_t begin, const int64_t end, int64_t &new_end);
protected:
  ObBM25ParamEstimator bm25_param_estimator_;
  sql::ObMatchAgainstMode mode_flag_;
  bool function_lookup_mode_;
private:
  // With a few query tokens, dims are merged by scanning the current doc id of every unfinished
  // dim instead of maintaining the merge heap. Documents and the dims of each document are
  // visited in the same order as the heap pops them: by doc id, then by dim index.
  static const int64_t LINEAR_MERGE_MAX_DIM_CNT = 8;
  bool use_linear_merge_;
  common::ObDatumCmpFuncType id_cmp_func_;
  ObTextRetrievalDaaTTokenIter **token_iters_;
  double *dim_relevances_;
  int64_t *active_dims_; // ascending indexes of the dims not iterated to the end
  int64_t active_dim_cnt_;
  // Without a limit, rows are buffered up to the batch capacity before the score filter is
  // evaluated on all of them at once, instead of evaluating it row by row.
  bool use_batch_filter_;
  int64_t batch_capacity_; // capacity of the running get_next_rows()
  sql::ObBitVector *filter_skip_;
  DISALLOW_COPY_AND_ASSIGN(ObTextDaaTIter);
};


class ObTextBMWIter final : public ObSRBMWIterImpl
{
public:
  ObTextBMWIter();
  virtual ~ObTextBMWIter() { reset(); }
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
  int init(const ObTextDaaTParam &param);
protected:
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
  virtual int init_before_wand_process() override;
protected:
  ObBM25ParamEstimator bm25_param_estimator_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTextBMWIter);
};

} // namespace storage
} // namespace oceanbase

#endif
