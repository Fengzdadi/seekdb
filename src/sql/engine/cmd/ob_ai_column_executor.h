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

#include "lib/atomic/ob_atomic.h"
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace common
{
class ObSqlString;
}
namespace sql
{
class ObSQLSessionInfo;
class ObPhysicalPlan;

// Counts the AI column task writes of this process. The task table only shows the tasks once the
// writing transaction commits, so the scheduler watches the count to know when it is worth
// polling the table often.
class AIColumnTaskSignal
{
public:
  static void notify() { ATOMIC_INC(&seq_); }
  static int64_t get_seq() { return ATOMIC_LOAD(&seq_); }
private:
  inline static int64_t seq_ = 0;
};

// Statement-side operations on AI columns that run as inner SQL.
class AIColumnExecutor
{
public:
  // With ai_column_strict_read on, fail with OB_AI_COLUMN_NOT_READY when an AI column the plan
  // references has a row that is not READY. The check covers the whole column, not only the
  // rows the statement selects.
  static int check_strict_read(ObSQLSessionInfo &session, const ObPhysicalPlan &plan);
  // ALTER TABLE ... RETRY AI COLUMN column [WHERE condition]
  static int retry(ObSQLSessionInfo &session,
                   const common::ObString &database_name,
                   const common::ObString &table_name,
                   const common::ObString &column_name,
                   const common::ObString &condition);
  // regenerate every row of the AI columns added or redefined by ALTER TABLE
  static int refresh(ObSQLSessionInfo &session,
                     const common::ObString &database_name,
                     const common::ObString &table_name,
                     const common::ObIArray<common::ObString> &column_names);
  // replace the STATUS pseudo column of a RETRY condition by the state of the AI column;
  // quoted, backquoted and qualified names as well as function names are left alone
  static int rewrite_retry_condition(const common::ObString &condition,
                                     const common::ObString &state_column_name,
                                     common::ObSqlString &rewritten);
private:
  static int invalidate(ObSQLSessionInfo &session,
                        const common::ObString &database_name,
                        const common::ObString &table_name,
                        const common::ObIArray<common::ObString> &column_names,
                        const common::ObString &condition);
};

} // namespace sql
} // namespace oceanbase
