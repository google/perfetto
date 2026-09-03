/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_SQLITE_COMMITTED_STATE_MANAGER_H_
#define SRC_TRACE_PROCESSOR_SQLITE_COMMITTED_STATE_MANAGER_H_

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/ext/base/flat_hash_map.h"

namespace perfetto::trace_processor::sqlite {

// Storage for the committed view of per-vtab state. Composed into
// |sqlite::ModuleStateManager| so the committed view can live outside the
// per-connection manager (e.g. on PerfettoSqlDatabase, shared across every
// connection attached to that database).
class CommittedStateManager {
 public:
  CommittedStateManager() = default;
  CommittedStateManager(const CommittedStateManager&) = delete;
  CommittedStateManager& operator=(const CommittedStateManager&) = delete;

  std::shared_ptr<void> Load(std::string_view name) const {
    const auto* p = map_.Find(name);
    return p ? *p : nullptr;
  }
  void Store(std::string_view name, std::shared_ptr<void> state) {
    map_.Erase(name);
    map_.Insert(std::string(name), std::move(state));
  }
  void Erase(std::string_view name) { map_.Erase(name); }

 private:
  // SQL object names are case-insensitive, so the map must be too.
  base::FlatHashMapV2<std::string,
                      std::shared_ptr<void>,
                      base::CaseInsensitiveHash,
                      base::CaseInsensitiveEq>
      map_;
};

}  // namespace perfetto::trace_processor::sqlite

#endif  // SRC_TRACE_PROCESSOR_SQLITE_COMMITTED_STATE_MANAGER_H_
