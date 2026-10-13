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

#include "src/trace_processor/perfetto_sql/stdlib/stdlib.h"

#include <memory>
#include <mutex>

#include "perfetto/ext/base/no_destructor.h"
#include "src/trace_processor/perfetto_sql/stdlib/amalgamated_stdlib.h"
#include "src/trace_processor/util/sql_bundle.h"

namespace perfetto::trace_processor::stdlib {

std::shared_ptr<const SqlBundle> GetStdlibBundle() {
  struct Cache {
    std::mutex mutex;
    bool requested = false;
    std::weak_ptr<const SqlBundle> first;
    std::shared_ptr<const SqlBundle> retained;
  };
  static base::NoDestructor<Cache> cache;
  auto& state = cache.ref();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.retained)
    return state.retained;
  auto bundle = state.first.lock();
  if (!bundle) {
    bundle = std::make_shared<const SqlBundle>(SqlBundle::Decode(
        kAmalgamatedStdlib.data(), kAmalgamatedStdlib.size()));
  }
  if (state.requested) {
    // Reuse the first decode if its consumer is still alive; otherwise retain
    // this second decode. A one-shot consumer releases its buffer normally.
    state.retained = bundle;
  } else {
    state.first = bundle;
    state.requested = true;
  }
  return bundle;
}

}  // namespace perfetto::trace_processor::stdlib
