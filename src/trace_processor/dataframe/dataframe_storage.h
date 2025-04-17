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

#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_STORAGE_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_STORAGE_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/uuid.h"
#include "src/trace_processor/dataframe/dataframe.h"

namespace perfetto::trace_processor {

// TODO
class DataframeStorage {
 public:
  // Identifies a dataframe. See the `MakeTag` methods below.
  struct Tag {
    uint64_t hash;
  };

  // Checks whether a dataframe with the given tag has already been created.
  //
  // Returns nullptr if no such dataframe exists.
  std::shared_ptr<const Dataframe> Find(Tag tag) {
    std::lock_guard<std::mutex> mu(mutex_);
    auto* it = dataframes_.Find(tag.hash);
    if (!it) {
      return nullptr;
    }
    return it->lock();
  }

  // Inserts a dataframe into the shared storage to be associated with the given
  // tag.
  //
  // Returns the dataframe which is now owned by the shared storage. This might
  // be the same dataframe which was passed in as the argument or it might be a
  // a dataframe which is already stored in the shared storage.
  std::shared_ptr<const Dataframe> Insert(Tag tag,
                                          std::unique_ptr<Dataframe> df) {
    std::shared_ptr<Dataframe> shared_df(std::move(df));
    auto [it, inserted] = dataframes_.Insert(tag.hash, shared_df);
    if (inserted) {
      return shared_df;
    }
    std::shared_ptr<Dataframe> existing_shared_df = it->lock();
    if (existing_shared_df) {
      return existing_shared_df;
    }
    *it = shared_df;
    return shared_df;
  }

  static Tag MakeTagForSqlModuleTable(const std::string& module_name,
                                      const std::string& table_name) {
    return Tag{base::Hasher::Combine(module_name, table_name)};
  }
  static Tag MakeTagForStaticTable(const std::string& table_name) {
    return Tag{base::Hasher::Combine(table_name)};
  }
  static Tag MakeUniqueTag() {
    return Tag{base::Hasher::Combine(base::Uuidv4().ToPrettyString())};
  }

 private:
  using DataframeMap = base::FlatHashMap<uint64_t,
                                         std::unique_ptr<Dataframe>,
                                         base::AlreadyHashed<uint64_t>>;
  DataframeMap dataframes_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_STORAGE_H_
