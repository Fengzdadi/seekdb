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

#ifndef OCEANBASE_SHARE_AUTOINCREMENT_OB_I_TABLET_AUTOINCREMENT_SERVICE_H_
#define OCEANBASE_SHARE_AUTOINCREMENT_OB_I_TABLET_AUTOINCREMENT_SERVICE_H_

#include <stdint.h>

namespace oceanbase
{
namespace common
{
class ObTabletID;
}
namespace share
{
struct ObTabletCacheInterval;

// Stable allocation interface for callers that need a value but must not know
// about the Storage cache, leader lookup, RPC, retry, or persistence details.
class ObITabletAutoincrementService
{
public:
  virtual ~ObITabletAutoincrementService() {}

  virtual int next_value(
      const common::ObTabletID &tablet_id,
      uint64_t &value) = 0;
  // Reserves a range of at least count values, outside the shared cache, for
  // the caller to hand out on its own.
  virtual int next_interval(
      const common::ObTabletID &tablet_id,
      const uint64_t count,
      ObTabletCacheInterval &interval) = 0;
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AUTOINCREMENT_OB_I_TABLET_AUTOINCREMENT_SERVICE_H_
