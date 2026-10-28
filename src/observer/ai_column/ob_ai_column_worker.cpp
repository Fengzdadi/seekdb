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

#define USING_LOG_PREFIX SERVER

#include "observer/ai_column/ob_ai_column_worker.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/oblog/ob_warning_buffer.h"
#include "lib/random/ob_random.h"
#include "lib/string/ob_sql_string.h"
#include "lib/thread/ob_thread_name.h"
#include "lib/worker.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/cmd/ob_ai_column_executor.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace observer
{
namespace
{
const char *const TASK_TABLE = "oceanbase.__all_ai_column_task";
const int64_t MAX_ERROR_LENGTH = 1024;

int append_name(const ObString &name, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    const char c = name.ptr()[i];
    ret = '`' == c ? sql.append("``") : sql.append(&c, 1);
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("`"))) {
  }
  return ret;
}

int append_table_name(const ObString &database_name, const ObString &table_name, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_name(database_name, sql))) {
  } else if (OB_FAIL(sql.append("."))) {
  } else if (OB_FAIL(append_name(table_name, sql))) {
  }
  return ret;
}

int append_literal(const ObString &str, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < str.length(); ++i) {
    const char c = str.ptr()[i];
    if ('\'' == c) {
      ret = sql.append("''");
    } else if ('\\' == c) {
      ret = sql.append("\\\\");
    } else if ('\0' == c) {
      ret = sql.append("\\0");
    } else {
      ret = sql.append(&c, 1);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
  }
  return ret;
}

// a string column value; text values may carry a LOB locator
int get_text_value(const sqlclient::ObMySQLResult &result,
                   const int64_t idx,
                   ObIAllocator &allocator,
                   bool &is_null,
                   ObString &value)
{
  int ret = OB_SUCCESS;
  ObObj obj;
  ObString data;
  is_null = false;
  value.reset();
  if (OB_FAIL(result.get_obj(idx, obj))) {
    LOG_WARN("failed to get value", K(ret), K(idx));
  } else if (obj.is_null()) {
    is_null = true;
  } else if (obj.is_lob_storage() && obj.has_lob_header()) {
    ObLobLocatorV2 locator(obj.get_string(), true);
    if (OB_FAIL(locator.get_inrow_data(data))) {
      LOG_WARN("failed to read lob value", K(ret));
    }
  } else if (OB_UNLIKELY(!obj.is_string_type())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected value type", K(ret), K(obj));
  } else {
    data = obj.get_string();
  }
  if (OB_SUCC(ret) && !is_null && OB_FAIL(ob_write_string(allocator, data, value))) {
    LOG_WARN("failed to copy value", K(ret));
  }
  return ret;
}

int get_table(ObSchemaGetterGuard &guard,
              const uint64_t table_id,
              const ObTableSchema *&table_schema,
              ObString &database_name)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database_schema = NULL;
  table_schema = NULL;
  database_name.reset();
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    LOG_WARN("failed to get schema guard", K(ret));
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
    LOG_WARN("failed to get table schema", K(ret), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
  } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(), database_schema))) {
    LOG_WARN("failed to get database schema", K(ret));
  } else if (OB_ISNULL(database_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("database schema is null", K(ret), K(table_id));
  } else {
    database_name = database_schema->get_database_name_str();
  }
  return ret;
}

// the HTTP status of a failed request as reported by the AI function client, 0 if unknown
int64_t parse_http_status(const ObString &error)
{
  int64_t status = 0;
  const char *prefix = "http status code: ";
  const int64_t prefix_len = strlen(prefix);
  const char *pos = error.empty() ? NULL : strstr(error.ptr(), prefix);
  if (NULL != pos) {
    status = strtol(pos + prefix_len, NULL, 10);
  }
  return status;
}
} // namespace

AIColumnURLLimiter::AIColumnURLLimiter()
  : is_inited_(false), stop_(false), cond_(), allocator_("AIColumnLimit"), slots_()
{
}

int AIColumnURLLimiter::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(cond_.init(ObWaitEventIds::DEFAULT_COND_WAIT))) {
    LOG_WARN("failed to init cond", K(ret));
  } else {
    stop_ = false;
    is_inited_ = true;
  }
  return ret;
}

void AIColumnURLLimiter::destroy()
{
  if (is_inited_) {
    cond_.destroy();
    slots_.reset();
    allocator_.reset();
    is_inited_ = false;
  }
}

void AIColumnURLLimiter::stop()
{
  if (is_inited_) {
    ObThreadCondGuard guard(cond_);
    stop_ = true;
    cond_.broadcast();
  }
}

// called with cond_ locked
int AIColumnURLLimiter::get_slot(const ObString &url, int64_t &idx)
{
  int ret = OB_SUCCESS;
  idx = -1;
  for (int64_t i = 0; -1 == idx && i < slots_.count(); ++i) {
    if (slots_.at(i).url_ == url) {
      idx = i;
    }
  }
  if (-1 == idx) {
    Slot slot;
    if (OB_FAIL(ob_write_string(allocator_, url, slot.url_))) {
      LOG_WARN("failed to copy url", K(ret));
    } else if (OB_FAIL(slots_.push_back(slot))) {
      LOG_WARN("failed to add url slot", K(ret));
    } else {
      idx = slots_.count() - 1;
    }
  }
  return ret;
}

int AIColumnURLLimiter::acquire(const ObString &url, int64_t &epoch)
{
  int ret = OB_SUCCESS;
  int64_t idx = -1;
  epoch = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else {
    ObThreadCondGuard guard(cond_);
    if (OB_FAIL(get_slot(url, idx))) {
    }
    while (OB_SUCC(ret) && !stop_ && slots_.at(idx).running_ >= slots_.at(idx).limit_) {
      (void)cond_.wait_us(100 * 1000);
    }
    if (OB_FAIL(ret)) {
    } else if (stop_) {
      ret = OB_IN_STOP_STATE;
    } else {
      ++slots_.at(idx).running_;
      epoch = slots_.at(idx).epoch_;
    }
  }
  return ret;
}

void AIColumnURLLimiter::release(const ObString &url, const int64_t epoch, const bool throttled)
{
  int ret = OB_SUCCESS;
  int64_t idx = -1;
  if (is_inited_) {
    ObThreadCondGuard guard(cond_);
    if (OB_FAIL(get_slot(url, idx))) {
      LOG_WARN("failed to get url slot", K(ret));
    } else {
      Slot &slot = slots_.at(idx);
      slot.running_ = slot.running_ > 0 ? slot.running_ - 1 : 0;
      if (!throttled) {
        if (++slot.successes_ >= slot.limit_ && slot.limit_ < MAX_LIMIT) {
          ++slot.limit_;
          slot.successes_ = 0;
        }
      } else if (epoch == slot.epoch_) {
        slot.limit_ = slot.limit_ > 1 ? slot.limit_ / 2 : 1;
        slot.successes_ = 0;
        ++slot.epoch_;
        LOG_INFO("ai column requests throttled", K(slot));
      }
      cond_.broadcast();
    }
  }
}

AIColumnWorker &AIColumnWorker::get_instance()
{
  static AIColumnWorker instance;
  return instance;
}

AIColumnWorker::AIColumnWorker()
  : lib::ThreadPool(1), is_inited_(false), executor_(), limiter_(), cond_(), claim_seq_(0)
{
  owner_prefix_[0] = '\0';
}

AIColumnWorker::~AIColumnWorker()
{
  destroy();
}

int AIColumnWorker::init()
{
  int ret = OB_SUCCESS;
  char addr[MAX_IP_PORT_LENGTH] = "";
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(cond_.init(ObWaitEventIds::THREAD_IDLING_COND_WAIT))) {
    LOG_WARN("failed to init cond", K(ret));
  } else if (OB_FAIL(limiter_.init())) {
    LOG_WARN("failed to init url limiter", K(ret));
  } else if (OB_FAIL(executor_.init(EXECUTOR_THREAD_COUNT, EXECUTOR_QUEUE_LIMIT, "AIColumnExec"))) {
    LOG_WARN("failed to init executor", K(ret));
  } else if (OB_FAIL(GCTX.self_addr().ip_port_to_string(addr, sizeof(addr)))) {
    LOG_WARN("failed to print server address", K(ret));
  } else {
    executor_.set_worker(this);
    snprintf(owner_prefix_, sizeof(owner_prefix_), "%s#%ld#", addr, ObTimeUtility::current_time());
    is_inited_ = true;
  }
  return ret;
}

int AIColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(lib::ThreadPool::start())) {
    LOG_WARN("failed to start ai column scheduler", K(ret));
  } else {
    LOG_INFO("ai column worker started", "owner_prefix", ObString::make_string(owner_prefix_));
  }
  return ret;
}

void AIColumnWorker::stop()
{
  if (is_inited_) {
    lib::ThreadPool::stop();
    limiter_.stop();
    {
      ObThreadCondGuard guard(cond_);
      cond_.broadcast();
    }
    executor_.stop();
  }
}

void AIColumnWorker::wait()
{
  if (is_inited_) {
    lib::ThreadPool::wait();
    executor_.wait();
  }
}

void AIColumnWorker::destroy()
{
  if (is_inited_) {
    executor_.destroy();
    lib::ThreadPool::destroy();
    limiter_.destroy();
    cond_.destroy();
    is_inited_ = false;
  }
}

void AIColumnWorker::Executor::handle(void *task)
{
  Job *job = static_cast<Job *>(task);
  if (OB_NOT_NULL(job)) {
    if (OB_NOT_NULL(worker_)) {
      worker_->handle_job(*job);
    }
    free_job(job);
  }
}

void AIColumnWorker::Executor::handle_drop(void *task)
{
  // the claim of a dropped task runs out with its lease and the task is picked up again
  free_job(static_cast<Job *>(task));
}

int AIColumnWorker::alloc_job(const JobType type,
                              const uint64_t table_id,
                              const ObString &predicate,
                              const uint64_t column_id,
                              const uint64_t request_id,
                              const ObString &owner,
                              Job *&job)
{
  int ret = OB_SUCCESS;
  const int64_t size = sizeof(Job) + predicate.length() + owner.length();
  char *buf = static_cast<char *>(ob_malloc(size, ObMemAttr("AIColumnJob")));
  job = NULL;
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to alloc ai column job", K(ret), K(size));
  } else {
    job = new (buf) Job();
    char *str = buf + sizeof(Job);
    if (predicate.length() > 0) {
      MEMCPY(str, predicate.ptr(), predicate.length());
    }
    job->predicate_.assign_ptr(str, predicate.length());
    str += predicate.length();
    if (owner.length() > 0) {
      MEMCPY(str, owner.ptr(), owner.length());
    }
    job->owner_.assign_ptr(str, owner.length());
    job->type_ = type;
    job->table_id_ = table_id;
    job->column_id_ = column_id;
    job->request_id_ = request_id;
  }
  return ret;
}

void AIColumnWorker::free_job(Job *job)
{
  if (OB_NOT_NULL(job)) {
    job->~Job();
    ob_free(job);
  }
}

void AIColumnWorker::handle_job(Job &job)
{
  int ret = OB_SUCCESS;
  // the AI function client reports details through the warning buffer of the thread
  ObWarningBuffer *saved_buffer = ob_get_tsi_warning_buffer();
  ObWarningBuffer *warning_buffer = OB_NEW(ObWarningBuffer, ObMemAttr("AIColumnWarn"));
  if (OB_NOT_NULL(warning_buffer)) {
    ob_setup_tsi_warning_buffer(warning_buffer);
  }
  if (JOB_TASK == job.type_) {
    if (OB_FAIL(process_task(job))) {
      LOG_WARN("failed to process ai column task", K(ret), K(job));
    }
  } else if (OB_FAIL(finalize_group(job.table_id_, job.predicate_, job.request_id_))) {
    LOG_WARN("failed to finalize ai column group", K(ret), K(job));
  }
  if (OB_NOT_NULL(warning_buffer)) {
    ob_setup_tsi_warning_buffer(saved_buffer);
    OB_DELETE(ObWarningBuffer, "AIColumnWarn", warning_buffer);
  }
}

int AIColumnWorker::wait_until_serving()
{
  int ret = OB_SUCCESS;
  while (!has_set_stop()
         && (share::SS_SERVING != GCTX.status_ || OB_ISNULL(GCTX.sql_proxy_)
             || OB_ISNULL(GCTX.schema_service_))) {
    ob_usleep(100 * 1000);
  }
  if (has_set_stop()) {
    ret = OB_IN_STOP_STATE;
  }
  return ret;
}

void AIColumnWorker::run1()
{
  int ret = OB_SUCCESS;
  int64_t last_sweep_ts = 0;
  int64_t last_claim_ts = 0;
  int64_t last_active_ts = 0;
  int64_t seen_seq = -1;
  bool recovered = false;
  lib::set_thread_name("AIColumnSched");
  if (OB_FAIL(wait_until_serving())) {
    LOG_INFO("ai column scheduler stops before serving", K(ret));
  }
  while (OB_SUCC(ret) && !has_set_stop()) {
    int tmp_ret = OB_SUCCESS;
    int64_t claimed_cnt = 0;
    const int64_t now = ObTimeUtility::current_time();
    const int64_t seq = sql::AIColumnTaskSignal::get_seq();
    if (seq != seen_seq) {
      // tasks were written meanwhile, they show up once their transactions commit
      seen_seq = seq;
      last_active_ts = now;
    }
    const bool active = now - last_active_ts < ACTIVE_PERIOD_US;
    if (!recovered) {
      // tasks left RUNNING by an earlier server start go back to PENDING first
      if (OB_SUCCESS != (tmp_ret = recover_tasks(true))) {
        LOG_WARN("failed to recover ai column tasks", K(tmp_ret));
      } else {
        recovered = true;
      }
    } else {
      if (now - last_sweep_ts >= (active ? SWEEP_INTERVAL_US : IDLE_SWEEP_INTERVAL_US)) {
        if (OB_SUCCESS != (tmp_ret = recover_tasks(false))) {
          LOG_WARN("failed to recover expired ai column tasks", K(tmp_ret));
        }
        if (OB_SUCCESS != (tmp_ret = sweep_finished_groups())) {
          LOG_WARN("failed to sweep finished ai column groups", K(tmp_ret));
        }
        last_sweep_ts = now;
      }
      if (!active && now - last_claim_ts < IDLE_POLL_INTERVAL_US) {
        // nothing was written lately, only waiting retries and recovered tasks become runnable
      } else if (executor_.get_queue_num() >= CLAIM_BATCH_SIZE) {
        // the executors are busy, claiming more would only make the tasks wait while RUNNING
      } else if (OB_SUCCESS != (tmp_ret = claim_tasks(claimed_cnt))) {
        LOG_WARN("failed to claim ai column tasks", K(tmp_ret));
      } else {
        last_claim_ts = now;
        if (claimed_cnt > 0) {
          last_active_ts = now;
        }
      }
    }
    if (claimed_cnt < CLAIM_BATCH_SIZE && !has_set_stop()) {
      ObThreadCondGuard guard(cond_);
      (void)cond_.wait_us(recovered ? POLL_INTERVAL_US : 1000 * 1000);
    }
  }
}

int AIColumnWorker::recover_tasks(const bool at_startup)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  // a task row locked by an open business transaction must not hold up the scheduler for long
  if (OB_FAIL(sql.assign_fmt("UPDATE /*+ query_timeout(%ld) */ %s SET state = %d, owner = NULL,"
                             " lease_expire_ts = NULL WHERE state = %d AND ",
                             SCHEDULER_STMT_TIMEOUT_US, TASK_TABLE,
                             static_cast<int>(TASK_PENDING), static_cast<int>(TASK_RUNNING)))) {
  } else if (at_startup) {
    ret = sql.append_fmt("(owner IS NULL OR owner NOT LIKE '%s%%')", owner_prefix_);
  } else {
    ret = sql.append("lease_expire_ts < now(6)");
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to recover ai column tasks", K(ret), K(sql));
  } else if (affected_rows > 0) {
    LOG_INFO("ai column tasks recovered", K(at_startup), K(affected_rows));
  }
  return ret;
}

int AIColumnWorker::claim_tasks(int64_t &claimed_cnt)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSqlString owner;
  int64_t affected_rows = 0;
  bool has_runnable = false;
  claimed_cnt = 0;
  // a read first, so that an idle server does not run a write transaction on every poll
  if (OB_FAIL(sql.assign_fmt("SELECT 1 FROM %s WHERE state IN (%d, %d) AND eligible_ts <= now(6)"
                             " LIMIT 1", TASK_TABLE, static_cast<int>(TASK_PENDING),
                             static_cast<int>(TASK_RETRY_WAIT)))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("failed to read runnable ai column tasks", K(ret));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", K(ret));
      } else if (OB_SUCC(result->next())) {
        has_runnable = true;
      } else if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  if (OB_FAIL(ret) || !has_runnable) {
  } else if (OB_FAIL(owner.assign_fmt("%s%ld", owner_prefix_, ++claim_seq_))) {
  } else if (OB_FAIL(sql.assign_fmt("UPDATE /*+ query_timeout(%ld) */ %s SET state = %d,"
                                    " owner = '%s',"
                                    " lease_expire_ts = date_add(now(6), INTERVAL %ld SECOND)"
                                    " WHERE state IN (%d, %d) AND eligible_ts <= now(6)"
                                    " ORDER BY eligible_ts, table_id, rowkey_predicate LIMIT %ld",
                                    SCHEDULER_STMT_TIMEOUT_US, TASK_TABLE,
                                    static_cast<int>(TASK_RUNNING), owner.ptr(),
                                    LEASE_SECONDS, static_cast<int>(TASK_PENDING),
                                    static_cast<int>(TASK_RETRY_WAIT), CLAIM_BATCH_SIZE))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to claim ai column tasks", K(ret), K(sql));
  } else if (affected_rows <= 0) {
  } else if (OB_FAIL(sql.assign_fmt("SELECT table_id, rowkey_predicate, column_id, request_id,"
                                    " definition_version, retry_count, max_retries FROM %s"
                                    " WHERE owner = '%s' AND state = %d", TASK_TABLE, owner.ptr(),
                                    static_cast<int>(TASK_RUNNING)))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("failed to read claimed ai column tasks", K(ret));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", K(ret));
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        int64_t table_id = 0;
        int64_t column_id = 0;
        uint64_t request_id = 0;
        ObString predicate;
        Job *job = NULL;
        int tmp_ret = OB_SUCCESS;
        EXTRACT_INT_FIELD_MYSQL(*result, "table_id", table_id, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "column_id", column_id, int64_t);
        EXTRACT_UINT_FIELD_MYSQL(*result, "request_id", request_id, uint64_t);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "rowkey_predicate", predicate);
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(alloc_job(JOB_TASK, table_id, predicate, column_id, request_id,
                                     owner.string(), job))) {
        } else {
          EXTRACT_INT_FIELD_MYSQL(*result, "definition_version", job->definition_version_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "retry_count", job->retry_count_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "max_retries", job->max_retries_, int64_t);
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(executor_.push(job))) {
            LOG_WARN("failed to push ai column task", K(ret), KPC(job));
          } else {
            job = NULL;
            ++claimed_cnt;
          }
          if (NULL != job) {
            if (OB_SUCCESS != (tmp_ret = release_task(*job))) {
              LOG_WARN("failed to release ai column task", K(tmp_ret), KPC(job));
            }
            free_job(job);
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

int AIColumnWorker::sweep_finished_groups()
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  // groups whose tasks are all final but were not published, e.g. after a restart or when
  // the row was locked; a group that just became final is being published by its executor
  if (executor_.get_queue_num() >= CLAIM_BATCH_SIZE) {
  } else if (OB_FAIL(sql.assign_fmt("SELECT table_id, rowkey_predicate, request_id FROM %s"
                                    " GROUP BY table_id, rowkey_predicate, request_id"
                                    " HAVING COUNT(CASE WHEN state NOT IN (%d, %d) THEN 1 END) = 0"
                                    " AND MAX(gmt_modified) < date_sub(now(6), INTERVAL %ld"
                                    " MICROSECOND) LIMIT %ld", TASK_TABLE,
                                    static_cast<int>(TASK_SUCCEEDED),
                                    static_cast<int>(TASK_FAILED), SWEEP_DELAY_US,
                                    CLAIM_BATCH_SIZE))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("failed to read finished ai column groups", K(ret));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", K(ret));
      }
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        int64_t table_id = 0;
        uint64_t request_id = 0;
        ObString predicate;
        Job *job = NULL;
        EXTRACT_INT_FIELD_MYSQL(*result, "table_id", table_id, int64_t);
        EXTRACT_UINT_FIELD_MYSQL(*result, "request_id", request_id, uint64_t);
        EXTRACT_VARCHAR_FIELD_MYSQL(*result, "rowkey_predicate", predicate);
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(alloc_job(JOB_FINALIZE, table_id, predicate, OB_INVALID_ID,
                                     request_id, ObString(), job))) {
        } else if (OB_FAIL(executor_.push(job))) {
          LOG_WARN("failed to push ai column finalize job", K(ret));
          free_job(job);
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

int AIColumnWorker::append_task_key(const uint64_t table_id,
                                    const ObString &predicate,
                                    const uint64_t request_id,
                                    ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt("table_id = %lu AND rowkey_predicate = ", table_id))) {
  } else if (OB_FAIL(append_literal(predicate, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" AND request_id = %lu", request_id))) {
  }
  return ret;
}

int AIColumnWorker::process_task(const Job &job)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColumnTask");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table_schema = NULL;
  ObString database_name;
  AIColumnMeta meta;
  const AIColumnDef *def = NULL;
  bool exists = false;
  bool is_null = false;
  bool recorded = false;
  ObString input;
  CallResult result;
  if (OB_FAIL(get_table(guard, job.table_id_, table_schema, database_name))) {
  } else if (OB_ISNULL(table_schema)) {
    // the table is gone and its tasks with it
    ret = delete_task(job);
  } else if (OB_FAIL(AIColumnSchemaUtil::get_meta(*table_schema, meta))) {
    LOG_WARN("failed to get ai column meta", K(ret), K(job));
  } else if (OB_ISNULL(def = meta.get_def(job.column_id_))
             || def->definition_version_ != job.definition_version_) {
    // the column was dropped or redefined, a redefinition queues tasks of its own
    if (OB_FAIL(delete_task(job))) {
    } else if (OB_FAIL(try_finalize_group(job.table_id_, job.predicate_, job.request_id_))) {
    }
  } else if (OB_FAIL(read_input(job, database_name, table_schema->get_table_name_str(), *def,
                                allocator, exists, is_null, input))) {
    if (!is_transient_error(ret)) {
      // the input cannot be computed, this counts as a failed attempt
      result.ret_ = ret;
      result.error_ = ObString::make_string("failed to compute the AI column input");
      ret = OB_SUCCESS;
      exists = true;
    }
  }
  if (OB_FAIL(ret) || OB_ISNULL(def) || !exists) {
    // nothing to do when a later change of the row replaced the task
  } else {
    if (OB_SUCCESS != result.ret_) {
    } else if (is_null) {
      // a NULL input gives a NULL value without asking the model
      result.is_null_ = true;
    } else if (input.empty()) {
      result.ret_ = OB_INVALID_ARGUMENT;
      result.terminal_ = true;
      result.error_ = ObString::make_string("the AI column input is empty");
    } else {
      call_model(*def, input, allocator, result);
    }
    if (OB_FAIL(record_result(job, result, recorded))) {
      LOG_WARN("failed to record ai column task result", K(ret), K(job));
    } else if (recorded && OB_FAIL(try_finalize_group(job.table_id_, job.predicate_,
                                                      job.request_id_))) {
      LOG_WARN("failed to finalize ai column group", K(ret), K(job));
    }
  }
  if (OB_FAIL(ret) && !recorded) {
    // the task could not be processed now, hand it back without using up a retry
    int tmp_ret = release_task(job);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("failed to release ai column task", K(tmp_ret), K(job));
    }
  }
  return ret;
}

int AIColumnWorker::read_input(const Job &job,
                               const ObString &database_name,
                               const ObString &table_name,
                               const AIColumnDef &def,
                               ObIAllocator &allocator,
                               bool &exists,
                               bool &is_null,
                               ObString &input)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  bool row_exists = false;
  int64_t task_cnt = 0;
  exists = false;
  is_null = false;
  input.reset();
  // the row and the task are read in one snapshot: while the task exists the row still holds
  // the version that created it, as a later change would have replaced the task
  if (OB_FAIL(sql.assign_fmt("SELECT CAST((%.*s) AS CHAR) AS ai_input, (SELECT COUNT(*) FROM %s"
                             " ai_task WHERE ai_task.table_id = %lu AND ai_task.rowkey_predicate = ",
                             def.prompt_expr_.length(), def.prompt_expr_.ptr(), TASK_TABLE,
                             job.table_id_))) {
  } else if (OB_FAIL(append_literal(job.predicate_, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" AND ai_task.column_id = %lu AND ai_task.request_id = %lu)"
                                    " AS ai_task_cnt FROM ", job.column_id_, job.request_id_))) {
  } else if (OB_FAIL(append_table_name(database_name, table_name, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" WHERE %.*s", job.predicate_.length(), job.predicate_.ptr()))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("failed to read ai column input", K(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("failed to read ai column input", K(ret), K(sql));
        }
      } else if (OB_FAIL(result->get_int(1L, task_cnt))) {
        LOG_WARN("failed to read task count", K(ret));
      } else if (OB_FAIL(get_text_value(*result, 0, allocator, is_null, input))) {
      } else {
        row_exists = true;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (row_exists) {
    exists = task_cnt > 0;
  } else if (OB_FAIL(delete_task(job))) {
    // the row is gone, so is any reason to keep its task
  }
  return ret;
}

void AIColumnWorker::call_model(const AIColumnDef &def,
                                const ObString &input,
                                ObIAllocator &allocator,
                                CallResult &result)
{
  int ret = OB_SUCCESS;
  ObAIFuncExprInfo *info = NULL;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  ObArray<ObString> headers;
  ObString access_key;
  ObString model;
  ObString content = input;
  ObString config_str;
  ObJsonObject *config = NULL;
  ObJsonObject *body = NULL;
  ObJsonObject *response = NULL;
  ObIJsonBase *output = NULL;
  if (OB_ISNULL(endpoint_resolver)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("endpoint resolver is null", K(ret));
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, def.model_name_, info))) {
    LOG_WARN("failed to get ai model", K(ret), K(def.model_name_));
  } else if (OB_ISNULL(info)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(def.model_name_, allocator,
                                                              endpoint, false))) {
    LOG_WARN("failed to resolve ai model endpoint", K(ret), K(def.model_name_));
  } else if (OB_FAIL(endpoint.get_unencrypted_access_key(allocator, access_key))) {
    LOG_WARN("failed to get access key", K(ret));
  } else if (!def.config_.empty()
             && (OB_FAIL(ob_write_string(allocator, def.config_, config_str))
                 || OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator, config_str,
                                                                        config)))) {
    LOG_WARN("failed to parse ai column request options", K(ret), K(def.config_));
  } else {
    model = endpoint.get_request_model_name().empty() ? info->model_
                                                      : endpoint.get_request_model_name();
    if (def.is_complete()) {
      ObAIFuncIComplete *provider = NULL;
      ObString system_prompt = def.type_prompt_;
      if (!ObAIFuncUtils::is_completion_type(info)) {
        ret = OB_INVALID_ARGUMENT;
        result.terminal_ = true;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI_COMPLETE, the model is not a completion model");
      } else if (OB_FAIL(ObAIFuncUtils::get_complete_provider(allocator, endpoint.get_provider(),
                                                              provider))) {
      } else if (OB_FAIL(provider->get_header(allocator, access_key, headers))) {
      } else if (OB_FAIL(provider->get_body(allocator, model, system_prompt, content, config,
                                            body))) {
      } else if (OB_FAIL(send_request(endpoint.get_url(), headers, body, allocator, response,
                                      result))) {
      } else if (OB_FAIL(provider->parse_output(allocator, response, output))) {
      } else if (OB_ISNULL(output)) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(allocator, output,
                                                              result.output_))) {
      }
    } else {
      ObAIFuncIEmbed *provider = NULL;
      ObArray<ObString> contents;
      ObJsonNode *data = NULL;
      ObIJsonBase *vector = NULL;
      if (!ObAIFuncUtils::is_dense_embedding_type(info)) {
        ret = OB_INVALID_ARGUMENT;
        result.terminal_ = true;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "AI_EMBED, the model is not a dense embedding model");
      } else if (OB_FAIL(ObAIFuncUtils::get_embed_provider(allocator, endpoint.get_provider(),
                                                           provider))) {
      } else if (OB_FAIL(contents.push_back(content))) {
      } else if (OB_FAIL(provider->get_header(allocator, access_key, headers))) {
      } else if (OB_FAIL(provider->get_body(allocator, model, contents, config, body))) {
      } else if (OB_FAIL(send_request(endpoint.get_url(), headers, body, allocator, response,
                                      result))) {
      } else if (OB_ISNULL(data = response->get_value("data"))
                 || ObJsonNodeType::J_ARRAY != data->json_type()) {
        // parse_output takes the data member for an array
        ret = OB_INVALID_DATA;
        LOG_WARN("unexpected embedding response", K(ret));
      } else if (OB_FAIL(provider->parse_output(allocator, response, output))) {
      } else if (OB_ISNULL(output) || ObJsonNodeType::J_ARRAY != output->json_type()
                 || 1 != output->element_count()) {
        ret = OB_INVALID_DATA;
        LOG_WARN("unexpected embedding count", K(ret));
      } else if (OB_FAIL(output->get_array_element(0, vector))) {
        LOG_WARN("failed to get embedding", K(ret));
      } else if (OB_ISNULL(vector) || ObJsonNodeType::J_ARRAY != vector->json_type()
                 || (def.vector_dimension_ > 0
                     && vector->element_count() != static_cast<uint64_t>(def.vector_dimension_))) {
        ret = OB_INVALID_DATA;
        LOG_WARN("unexpected embedding dimension", K(ret), K(def.vector_dimension_));
      } else if (OB_FAIL(ObAIFuncJsonUtils::print_json_to_str(allocator, vector,
                                                              result.output_))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
    ObString message = ob_get_tsi_err_msg(ret);
    char buf[MAX_ERROR_LENGTH];
    if (message.empty()) {
      snprintf(buf, sizeof(buf), "%s", ob_strerror(ret));
    } else {
      snprintf(buf, sizeof(buf), "%.*s", message.length(), message.ptr());
    }
    result.ret_ = ret;
    if (OB_SUCCESS != ob_write_string(allocator, ObString::make_string(buf), result.error_)) {
      result.error_ = ObString::make_string(ob_strerror(ret));
    }
    LOG_WARN("ai column model call failed", K(ret), K(def.column_name_), K(result.error_));
  }
}

int AIColumnWorker::send_request(const ObString &url,
                                 ObArray<ObString> &headers,
                                 ObJsonObject *body,
                                 ObIAllocator &allocator,
                                 ObJsonObject *&response,
                                 CallResult &result)
{
  int ret = OB_SUCCESS;
  ObAIFuncClient client;
  const int64_t saved_timeout_ts = THIS_WORKER.get_timeout_ts();
  ObWarningBuffer *warning_buffer = ob_get_tsi_warning_buffer();
  int64_t epoch = 0;
  if (OB_FAIL(limiter_.acquire(url, epoch))) {
    LOG_WARN("failed to wait for a request slot", K(ret), K(url));
  } else {
    // retries are scheduled by the task table, the client makes a single attempt
    client.set_max_retry_times(0);
    THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + REQUEST_TIMEOUT_US);
    if (OB_NOT_NULL(warning_buffer)) {
      warning_buffer->reset();
    }
    if (OB_FAIL(client.send_post(allocator, url, headers, body, response))) {
      LOG_WARN("ai column request failed", K(ret), K(url));
    } else if (OB_ISNULL(response) || ObJsonNodeType::J_OBJECT != response->json_type()) {
      // the client hands back any JSON document as an object
      ret = OB_INVALID_DATA;
      LOG_WARN("unexpected ai column response", K(ret), K(url));
    }
    THIS_WORKER.set_timeout_ts(saved_timeout_ts);
    result.throttled_ = OB_FAIL(ret) && 429 == parse_http_status(ob_get_tsi_err_msg(ret));
    limiter_.release(url, epoch, result.throttled_);
  }
  return ret;
}

int AIColumnWorker::record_result(const Job &job, const CallResult &result, bool &recorded)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  recorded = false;
  if (OB_SUCCESS == result.ret_) {
    if (OB_FAIL(sql.assign_fmt("UPDATE %s SET state = %d, result = ", TASK_TABLE,
                               static_cast<int>(TASK_SUCCEEDED)))) {
    } else if (result.is_null_ ? OB_FAIL(sql.append("NULL"))
                               : OB_FAIL(append_literal(result.output_, sql))) {
    } else if (OB_FAIL(sql.append(", last_error = NULL, lease_expire_ts = NULL"))) {
    }
  } else {
    const ObString error = result.error_.length() > MAX_ERROR_LENGTH
                           ? ObString(MAX_ERROR_LENGTH, result.error_.ptr()) : result.error_;
    if (result.terminal_) {
      ret = sql.assign_fmt("UPDATE %s SET state = %d", TASK_TABLE, static_cast<int>(TASK_FAILED));
    } else if (result.throttled_) {
      // the service asks to slow down, which says nothing about the request: send it again
      // later without using up a retry
      ret = sql.assign_fmt("UPDATE %s SET state = %d, eligible_ts = date_add(now(6), INTERVAL %ld"
                           " MICROSECOND), owner = NULL", TASK_TABLE,
                           static_cast<int>(TASK_RETRY_WAIT),
                           THROTTLE_BACKOFF_US + ObRandom::rand(0, THROTTLE_BACKOFF_US));
    } else {
      const int64_t backoff_us = get_backoff_us(job.retry_count_);
      // the state is assigned first: it must see the retry count before the increment
      ret = sql.assign_fmt("UPDATE %s SET state = CASE WHEN retry_count < max_retries THEN %d"
                           " ELSE %d END, retry_count = CASE WHEN retry_count < max_retries"
                           " THEN retry_count + 1 ELSE retry_count END,"
                           " eligible_ts = date_add(now(6), INTERVAL %ld MICROSECOND), owner = NULL",
                           TASK_TABLE, static_cast<int>(TASK_RETRY_WAIT),
                           static_cast<int>(TASK_FAILED), backoff_us);
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append(", lease_expire_ts = NULL, last_error = "))) {
    } else if (OB_FAIL(append_literal(error, sql))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(append_task_key(job.table_id_, job.predicate_, job.request_id_, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" AND column_id = %lu AND state = %d AND owner = ",
                                    job.column_id_, static_cast<int>(TASK_RUNNING)))) {
  } else if (OB_FAIL(append_literal(job.owner_, sql))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to record ai column task result", K(ret));
  } else {
    // no row when the task was replaced or taken over meanwhile, the outcome is dropped
    recorded = affected_rows > 0;
  }
  return ret;
}

int AIColumnWorker::release_task(const Job &job)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt("UPDATE %s SET state = %d, owner = NULL, lease_expire_ts = NULL,"
                             " eligible_ts = date_add(now(6), INTERVAL %ld MICROSECOND) WHERE ",
                             TASK_TABLE, static_cast<int>(TASK_PENDING), RELEASE_DELAY_US))) {
  } else if (OB_FAIL(append_task_key(job.table_id_, job.predicate_, job.request_id_, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" AND column_id = %lu AND state = %d AND owner = ",
                                    job.column_id_, static_cast<int>(TASK_RUNNING)))) {
  } else if (OB_FAIL(append_literal(job.owner_, sql))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to release ai column task", K(ret));
  }
  return ret;
}

int AIColumnWorker::delete_task(const Job &job)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE ", TASK_TABLE))) {
  } else if (OB_FAIL(append_task_key(job.table_id_, job.predicate_, job.request_id_, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" AND column_id = %lu", job.column_id_))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to delete ai column task", K(ret));
  }
  return ret;
}

int AIColumnWorker::try_finalize_group(const uint64_t table_id,
                                       const ObString &predicate,
                                       const uint64_t request_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t total_cnt = 0;
  int64_t open_cnt = 0;
  if (OB_FAIL(sql.assign_fmt("SELECT COUNT(*) AS total_cnt, COUNT(CASE WHEN state NOT IN (%d, %d)"
                             " THEN 1 END) AS open_cnt FROM %s WHERE ",
                             static_cast<int>(TASK_SUCCEEDED), static_cast<int>(TASK_FAILED),
                             TASK_TABLE))) {
  } else if (OB_FAIL(append_task_key(table_id, predicate, request_id, sql))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
        LOG_WARN("failed to read ai column group", K(ret));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result is null", K(ret));
      } else if (OB_FAIL(result->next())) {
        LOG_WARN("failed to read ai column group", K(ret));
      } else {
        EXTRACT_INT_FIELD_MYSQL(*result, "total_cnt", total_cnt, int64_t);
        EXTRACT_INT_FIELD_MYSQL(*result, "open_cnt", open_cnt, int64_t);
      }
    }
  }
  if (OB_SUCC(ret) && total_cnt > 0 && 0 == open_cnt
      && OB_FAIL(finalize_group(table_id, predicate, request_id))) {
  }
  return ret;
}

int AIColumnWorker::finalize_group(const uint64_t table_id,
                                   const ObString &predicate,
                                   const uint64_t request_id)
{
  int ret = OB_SUCCESS;
  bool done = false;
  for (int64_t attempt = 0; OB_SUCC(ret) && !done && attempt < MAX_PUBLISH_ATTEMPTS; ++attempt) {
    ObArenaAllocator allocator("AIColumnPub");
    ObSchemaGetterGuard guard;
    const ObTableSchema *table_schema = NULL;
    ObString database_name;
    AIColumnMeta meta;
    ObMySQLTransaction trans;
    ObSqlString sql;
    ObSqlString task_key;
    ObSqlString table_name;
    int64_t affected_rows = 0;
    bool row_exists = false;
    bool group_open = false;
    // final tasks of the group: column id, state and definition version
    ObSEArray<int64_t, 4> column_ids;
    ObSEArray<int64_t, 4> states;
    ObSEArray<int64_t, 4> versions;
    uint64_t failed_column_id = OB_INVALID_ID;
    ObString failed_error;
    if (OB_FAIL(get_table(guard, table_id, table_schema, database_name))) {
    } else if (NULL != table_schema && OB_FAIL(AIColumnSchemaUtil::get_meta(*table_schema, meta))) {
      LOG_WARN("failed to get ai column meta", K(ret));
    } else if (OB_FAIL(append_task_key(table_id, predicate, request_id, task_key))) {
    } else if (NULL != table_schema
               && OB_FAIL(append_table_name(database_name, table_schema->get_table_name_str(),
                                            table_name))) {
    } else if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
      LOG_WARN("failed to start transaction", K(ret));
    }
    // lock the row before the tasks, the order business DML takes the locks in
    if (OB_FAIL(ret) || NULL == table_schema) {
    } else if (OB_FAIL(sql.assign_fmt("SELECT /*+ query_timeout(%ld) */ 1 FROM %.*s WHERE %.*s"
                                      " FOR UPDATE", PUBLISH_LOCK_TIMEOUT_US,
                                      static_cast<int>(table_name.length()), table_name.ptr(),
                                      predicate.length(), predicate.ptr()))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        sqlclient::ObMySQLResult *result = NULL;
        if (OB_FAIL(trans.read(res, sql.ptr()))) {
          LOG_WARN("failed to lock ai column row", K(ret), K(sql));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("result is null", K(ret));
        } else if (OB_SUCC(result->next())) {
          row_exists = true;
        } else if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
    if (OB_FAIL(ret) || !row_exists) {
    } else if (OB_FAIL(sql.assign_fmt("SELECT column_id, state, definition_version FROM %s WHERE %.*s"
                                      " FOR UPDATE", TASK_TABLE,
                                      static_cast<int>(task_key.length()), task_key.ptr()))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        sqlclient::ObMySQLResult *result = NULL;
        if (OB_FAIL(trans.read(res, sql.ptr()))) {
          LOG_WARN("failed to lock ai column tasks", K(ret), K(sql));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("result is null", K(ret));
        }
        while (OB_SUCC(ret) && OB_SUCC(result->next())) {
          int64_t column_id = 0;
          int64_t state = 0;
          int64_t version = 0;
          EXTRACT_INT_FIELD_MYSQL(*result, "column_id", column_id, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "state", state, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "definition_version", version, int64_t);
          if (OB_FAIL(ret)) {
          } else if (TASK_SUCCEEDED != state && TASK_FAILED != state) {
            group_open = true;
          } else if (OB_FAIL(column_ids.push_back(column_id))) {
          } else if (OB_FAIL(states.push_back(state))) {
          } else if (OB_FAIL(versions.push_back(version))) {
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
    // publish values and states of the group, the request ids stay as they are so that the
    // DML task hook ignores this update
    for (int64_t i = 0; OB_SUCC(ret) && row_exists && !group_open
                        && OB_INVALID_ID == failed_column_id && i < column_ids.count(); ++i) {
      const AIColumnDef *def = meta.get_def(column_ids.at(i));
      const bool succeeded = TASK_SUCCEEDED == states.at(i);
      if (OB_ISNULL(def) || def->definition_version_ != versions.at(i)) {
        // the definition changed, the value belongs to the new definition's tasks
      } else if (OB_FAIL(sql.assign_fmt("UPDATE /*+ opt_param('hidden_column_visible', 'true') */"
                                        " %.*s SET ", static_cast<int>(table_name.length()),
                                        table_name.ptr()))) {
      } else if (OB_FAIL(append_name(def->column_name_, sql))) {
      } else if (succeeded
                 && OB_FAIL(sql.append_fmt(" = (SELECT result FROM %s WHERE %.*s AND column_id = %ld)",
                                           TASK_TABLE, static_cast<int>(task_key.length()),
                                           task_key.ptr(), column_ids.at(i)))) {
      } else if (!succeeded && OB_FAIL(sql.append(" = NULL"))) {
      } else if (OB_FAIL(sql.append(", "))) {
      } else if (OB_FAIL(append_name(def->state_column_name_, sql))) {
      } else if (OB_FAIL(sql.append_fmt(" = %d, ", static_cast<int>(succeeded ? AI_COLUMN_STATE_READY
                                                                               : AI_COLUMN_STATE_FAILED)))) {
      } else if (OB_FAIL(append_name(def->spec_column_name_, sql))) {
      } else if (OB_FAIL(sql.append_fmt(" = %ld WHERE %.*s", def->definition_version_,
                                        predicate.length(), predicate.ptr()))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
        if (succeeded && !is_transient_error(ret)) {
          // the model answer does not convert to the column type, a failed attempt
          ObString message = ob_get_tsi_err_msg(ret);
          LOG_WARN("ai column value does not fit the column", K(ret), K(def->column_name_));
          failed_column_id = column_ids.at(i);
          if (OB_SUCCESS != ob_write_string(allocator, message.empty()
                                                       ? ObString::make_string(ob_strerror(ret))
                                                       : message, failed_error)) {
            failed_error = ObString::make_string("invalid AI column value");
          }
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("failed to publish ai column value", K(ret), K(sql));
        }
      }
    }
    if (OB_SUCC(ret) && trans.is_started() && OB_INVALID_ID == failed_column_id && !group_open) {
      // the group is published or its row is gone, either way its tasks are done
      if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE %.*s", TASK_TABLE,
                                 static_cast<int>(task_key.length()), task_key.ptr()))) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
        LOG_WARN("failed to delete ai column tasks", K(ret));
      }
    }
    if (trans.is_started()) {
      const bool commit = OB_SUCC(ret) && OB_INVALID_ID == failed_column_id && !group_open;
      int tmp_ret = trans.end(commit);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("failed to end ai column publish transaction", K(tmp_ret), K(commit));
        ret = OB_SUCCESS == ret ? tmp_ret : ret;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (NULL == table_schema) {
      // the table is gone, drop what is left of its group
      if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE %.*s", TASK_TABLE,
                                 static_cast<int>(task_key.length()), task_key.ptr()))) {
      } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
        LOG_WARN("failed to delete ai column tasks", K(ret));
      }
      done = true;
    } else if (OB_INVALID_ID != failed_column_id) {
      // count the conversion failure as an attempt and look at the group again
      if (OB_FAIL(fail_published_task(table_id, predicate, failed_column_id, request_id,
                                      failed_error))) {
        LOG_WARN("failed to record ai column conversion failure", K(ret));
      }
    } else {
      done = true;
    }
  }
  return ret;
}

int AIColumnWorker::fail_published_task(const uint64_t table_id,
                                        const ObString &predicate,
                                        const uint64_t column_id,
                                        const uint64_t request_id,
                                        const ObString &error)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  const ObString message = error.length() > MAX_ERROR_LENGTH
                           ? ObString(MAX_ERROR_LENGTH, error.ptr()) : error;
  if (OB_FAIL(sql.assign_fmt("UPDATE %s SET state = CASE WHEN retry_count < max_retries THEN %d"
                             " ELSE %d END, retry_count = CASE WHEN retry_count < max_retries"
                             " THEN retry_count + 1 ELSE retry_count END,"
                             " eligible_ts = date_add(now(6), INTERVAL %ld MICROSECOND),"
                             " owner = NULL, result = NULL, last_error = ", TASK_TABLE,
                             static_cast<int>(TASK_RETRY_WAIT), static_cast<int>(TASK_FAILED),
                             RETRY_BACKOFF_US))) {
  } else if (OB_FAIL(append_literal(message, sql))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(append_task_key(table_id, predicate, request_id, sql))) {
  } else if (OB_FAIL(sql.append_fmt(" AND column_id = %lu AND state = %d", column_id,
                                    static_cast<int>(TASK_SUCCEEDED)))) {
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql.ptr(), affected_rows))) {
    LOG_WARN("failed to fail ai column task", K(ret));
  }
  return ret;
}

int64_t AIColumnWorker::get_backoff_us(const int64_t retry_count)
{
  int64_t backoff_us = RETRY_BACKOFF_US;
  for (int64_t i = 0; i < retry_count && backoff_us < MAX_RETRY_BACKOFF_US; ++i) {
    backoff_us *= 2;
  }
  backoff_us = backoff_us < MAX_RETRY_BACKOFF_US ? backoff_us : MAX_RETRY_BACKOFF_US;
  // jitter keeps the retries of a failed burst from arriving together
  return backoff_us + ObRandom::rand(0, backoff_us / 4);
}

bool AIColumnWorker::is_transient_error(const int err)
{
  return OB_TIMEOUT == err || OB_TRANS_TIMEOUT == err || OB_TRANS_STMT_TIMEOUT == err
         || OB_TRY_LOCK_ROW_CONFLICT == err || OB_TRANSACTION_SET_VIOLATION == err
         || OB_ERR_EXCLUSIVE_LOCK_CONFLICT == err || OB_ERR_SHARED_LOCK_CONFLICT == err
         || OB_SCHEMA_EAGAIN == err || OB_SCHEMA_ERROR == err || OB_TABLE_NOT_EXIST == err
         || OB_ALLOCATE_MEMORY_FAILED == err || OB_EAGAIN == err || OB_NOT_MASTER == err
         || OB_IN_STOP_STATE == err;
}

} // namespace observer
} // namespace oceanbase
