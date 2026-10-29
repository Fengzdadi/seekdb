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

#include "ob_abort.h"
#include "lib/ob_define.h"
#include "lib/oblog/ob_log.h"
#ifdef _WIN32
#include <windows.h>
#endif

#if defined(_MSC_VER)
#define OB_NORETURN __declspec(noreturn)
#elif defined(__GNUC__) || defined(__clang__)
#define OB_NORETURN __attribute__((noreturn))
#else
#define OB_NORETURN
#endif

OB_NORETURN OB_WEAK_SYMBOL void ob_abort (void) __THROW
{
#ifdef _WIN32
  {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "OB_ABORT called, tid: %ld\r\n", GETTID());
    HANDLE h = GetStdHandle((DWORD)-12); /* STD_ERROR_HANDLE */
    DWORD written;
    WriteFile(h, buf, (DWORD)n, &written, NULL);
  }
#endif
  fprintf(stderr, "OB_ABORT, tid: %ld, lbt: %s\n", GETTID(), oceanbase::common::lbt());
  abort();
}

void ob_assert_fail (const char *exp, const char *file, const int line, const char *function) __THROW
{
  // the same two logs as OB_ASSERT printed in place, at the location of the assertion
  if (OB_LOG_NEED_TO_PRINT(ERROR)) {
    OB_LOGGER.log_message_fmt("", OB_LOG_LEVEL_ERROR, file, line, function, oceanbase::common::OB_ERROR,
                              "assert fail, exp=%s", exp);
  }
  if (OB_LOG_NEED_TO_PRINT(ERROR)) {
    OB_LOGGER.log_message_fmt("", OB_LOG_LEVEL_ERROR, file, line, function, oceanbase::common::OB_ERROR,
                              "assert fail BACKTRACE:%s", oceanbase::common::lbt());
  }
  ob_abort();
}
