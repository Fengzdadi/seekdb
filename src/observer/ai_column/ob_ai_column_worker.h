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

#include "lib/allocator/page_arena.h"
#include "lib/container/ob_array.h"
#include "lib/container/ob_se_array.h"
#include "lib/lock/ob_thread_cond.h"
#include "lib/string/ob_string.h"
#include "lib/thread/ob_simple_thread_pool.h"
#include "lib/thread/thread_pool.h"

namespace oceanbase
{
namespace common
{
class ObSqlString;
class ObJsonObject;
}
namespace share
{
namespace schema
{
struct AIColumnDef;
}
}
namespace observer
{

// Bounds the number of concurrent model requests per URL. Endpoints that share a URL share
// the bound. The bound halves when the service answers 429 and grows back by one after as many
// successes as the bound; a burst of 429 answers to requests sent under the same bound halves
// it only once.
class AIColumnURLLimiter
{
public:
  static const int64_t INITIAL_LIMIT = 8;
  static const int64_t MAX_LIMIT = 32;

  AIColumnURLLimiter();
  ~AIColumnURLLimiter() { destroy(); }
  int init();
  void destroy();
  // blocks until a request to url may be sent; fails only when the limiter stops.
  // epoch identifies the bound the request is sent under and goes back to release.
  int acquire(const common::ObString &url, int64_t &epoch);
  void release(const common::ObString &url, const int64_t epoch, const bool throttled);
  void stop();

private:
  struct Slot
  {
    Slot() : url_(), limit_(INITIAL_LIMIT), running_(0), successes_(0), epoch_(0) {}
    TO_STRING_KV(K_(url), K_(limit), K_(running), K_(successes), K_(epoch));
    common::ObString url_;
    int64_t limit_;
    int64_t running_;
    int64_t successes_;
    // bumped whenever the bound shrinks
    int64_t epoch_;
  };
  int get_slot(const common::ObString &url, int64_t &idx);

  bool is_inited_;
  bool stop_;
  common::ObThreadCond cond_;
  common::ObArenaAllocator allocator_;
  common::ObSEArray<Slot, 8> slots_;
};

// Background service that turns __all_ai_column_task rows into AI column values.
//
// The scheduler thread claims runnable tasks (PENDING, or RETRY_WAIT whose backoff is over)
// by setting them RUNNING with a claim token and hands them to the executor pool. An executor
// reads the input of the row version the task belongs to, calls the model and stores the
// outcome in the task row. Once every task of a group (the AI columns invalidated together by
// one row change, sharing a request id) is final, the results are published to the row and the
// group's tasks are removed in one transaction. Publishing locks the business row before the
// task rows, the order business DML uses, and only happens while the group's task rows still
// exist: a later change of the row replaces them, so late results are dropped.
class AIColumnWorker : public lib::ThreadPool
{
public:
  static AIColumnWorker &get_instance();

  AIColumnWorker();
  virtual ~AIColumnWorker();
  int init();
  int start() override;
  void stop() override;
  void wait() override;
  void destroy();
  void run1() override;

  // task states of __all_ai_column_task
  enum TaskState
  {
    TASK_PENDING = 0,
    TASK_RUNNING = 1,
    TASK_SUCCEEDED = 2,
    TASK_RETRY_WAIT = 3,
    TASK_FAILED = 4,
  };

private:
  static const int64_t EXECUTOR_THREAD_COUNT = 32;
  static const int64_t EXECUTOR_QUEUE_LIMIT = 4096;
  static const int64_t CLAIM_BATCH_SIZE = 64;
  // the scheduler looks for runnable tasks every POLL_INTERVAL_US for ACTIVE_PERIOD_US after
  // tasks were written or claimed, and every IDLE_POLL_INTERVAL_US otherwise
  static const int64_t POLL_INTERVAL_US = 10 * 1000;
  static const int64_t IDLE_POLL_INTERVAL_US = 1000 * 1000;
  static const int64_t ACTIVE_PERIOD_US = 10 * 1000 * 1000;
  static const int64_t SWEEP_INTERVAL_US = 1000 * 1000;
  static const int64_t IDLE_SWEEP_INTERVAL_US = 5 * 1000 * 1000;
  static const int64_t LEASE_SECONDS = 600;
  static const int64_t REQUEST_TIMEOUT_US = 30 * 1000 * 1000;
  static const int64_t RETRY_BACKOFF_US = 1000 * 1000;
  // a throttled request is sent again after this delay plus jitter, without using up a retry
  static const int64_t THROTTLE_BACKOFF_US = 500 * 1000;
  static const int64_t MAX_RETRY_BACKOFF_US = 60 * 1000 * 1000;
  static const int64_t RELEASE_DELAY_US = 1000 * 1000;
  static const int64_t MAX_PUBLISH_ATTEMPTS = 4;
  static const int64_t SCHEDULER_STMT_TIMEOUT_US = 3 * 1000 * 1000;
  // a row locked by a business transaction is published by a later sweep
  static const int64_t PUBLISH_LOCK_TIMEOUT_US = 5 * 1000 * 1000;
  // the sweep leaves a group alone this long after its last task change
  static const int64_t SWEEP_DELAY_US = 2 * 1000 * 1000;

  enum JobType
  {
    JOB_TASK = 0,
    JOB_FINALIZE = 1,
  };
  // one claimed task or one group to finalize; strings point into the job allocation
  struct Job
  {
    Job() : type_(JOB_TASK), table_id_(common::OB_INVALID_ID), predicate_(),
            column_id_(common::OB_INVALID_ID), request_id_(0), definition_version_(0),
            retry_count_(0), max_retries_(0), owner_() {}
    TO_STRING_KV(K_(type), K_(table_id), K_(predicate), K_(column_id), K_(request_id),
                 K_(definition_version), K_(retry_count), K_(max_retries), K_(owner));
    JobType type_;
    uint64_t table_id_;
    common::ObString predicate_;
    uint64_t column_id_;
    uint64_t request_id_;
    int64_t definition_version_;
    int64_t retry_count_;
    int64_t max_retries_;
    common::ObString owner_;
  };
  class Executor : public common::ObSimpleThreadPool
  {
  public:
    Executor() : worker_(NULL) {}
    void set_worker(AIColumnWorker *worker) { worker_ = worker; }
  protected:
    void handle(void *task) override;
    void handle_drop(void *task) override;
  private:
    AIColumnWorker *worker_;
  };
  // outcome of one model call
  struct CallResult
  {
    CallResult() : ret_(common::OB_SUCCESS), is_null_(false), terminal_(false),
                   throttled_(false), output_(), error_() {}
    int ret_;
    bool is_null_;
    // the failure cannot go away by retrying
    bool terminal_;
    bool throttled_;
    common::ObString output_;
    common::ObString error_;
  };

  static int alloc_job(const JobType type,
                       const uint64_t table_id,
                       const common::ObString &predicate,
                       const uint64_t column_id,
                       const uint64_t request_id,
                       const common::ObString &owner,
                       Job *&job);
  static void free_job(Job *job);

  void handle_job(Job &job);
  int wait_until_serving();
  int recover_tasks(const bool at_startup);
  int claim_tasks(int64_t &claimed_cnt);
  int sweep_finished_groups();
  int process_task(const Job &job);
  int finalize_group(const uint64_t table_id,
                     const common::ObString &predicate,
                     const uint64_t request_id);
  int try_finalize_group(const uint64_t table_id,
                         const common::ObString &predicate,
                         const uint64_t request_id);
  int read_input(const Job &job,
                 const common::ObString &database_name,
                 const common::ObString &table_name,
                 const share::schema::AIColumnDef &def,
                 common::ObIAllocator &allocator,
                 bool &exists,
                 bool &is_null,
                 common::ObString &input);
  void call_model(const share::schema::AIColumnDef &def,
                  const common::ObString &input,
                  common::ObIAllocator &allocator,
                  CallResult &result);
  int send_request(const common::ObString &url,
                   common::ObArray<common::ObString> &headers,
                   common::ObJsonObject *body,
                   common::ObIAllocator &allocator,
                   common::ObJsonObject *&response,
                   CallResult &result);
  int record_result(const Job &job, const CallResult &result, bool &recorded);
  int release_task(const Job &job);
  int delete_task(const Job &job);
  int fail_published_task(const uint64_t table_id,
                          const common::ObString &predicate,
                          const uint64_t column_id,
                          const uint64_t request_id,
                          const common::ObString &error);
  int append_task_key(const uint64_t table_id,
                      const common::ObString &predicate,
                      const uint64_t request_id,
                      common::ObSqlString &sql);
  static int64_t get_backoff_us(const int64_t retry_count);
  static bool is_transient_error(const int err);

  bool is_inited_;
  Executor executor_;
  AIColumnURLLimiter limiter_;
  common::ObThreadCond cond_;
  // prefix of every claim token issued by this process, unique per server start
  char owner_prefix_[128];
  int64_t claim_seq_;
};

} // namespace observer
} // namespace oceanbase
