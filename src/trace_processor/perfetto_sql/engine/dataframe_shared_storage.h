/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_SHARED_STORAGE_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_SHARED_STORAGE_H_

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "perfetto/base/thread_annotations.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/dataframe/dataframe.h"

namespace perfetto::trace_processor {

class DataframeSharedStorage {
 public:
  std::shared_ptr<dataframe::Dataframe> Find(const std::string& tag) {
    std::lock_guard mu(mutex_);
    auto* it = dataframes_.Find(tag);
    if (!it) {
      return nullptr;
    }
    return it->lock();
  }

  std::shared_ptr<dataframe::Dataframe> Insert(
      const std::string& tag,
      std::unique_ptr<dataframe::Dataframe> df) {
    std::shared_ptr<dataframe::Dataframe> shared_df(std::move(df));
    std::lock_guard mu(mutex_);
    auto [it, inserted] = dataframes_.Insert(tag, shared_df);
    if (inserted) {
      return shared_df;
    }
    std::shared_ptr<dataframe::Dataframe> existing_shared_df = it->lock();
    if (existing_shared_df) {
      return existing_shared_df;
    }
    *it = shared_df;
    return shared_df;
  }

 private:
  using DataframeMap =
      base::FlatHashMap<std::string, std::weak_ptr<dataframe::Dataframe>>;

  std::mutex mutex_;
  DataframeMap dataframes_ PERFETTO_GUARDED_BY(mutex_);
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_ENGINE_DATAFRAME_SHARED_STORAGE_H_
