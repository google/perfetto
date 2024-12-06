/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ETM_MAPPING_VERSION_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ETM_MAPPING_VERSION_H_

#include <cstdint>

#include "src/trace_processor/importers/common/address_range.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"

namespace perfetto::trace_processor::etm {
class MappingVersion {
 public:
  MappingVersion(int64_t create_ts,
                 tables::StackProfileMappingTable::ConstRowReference mapping)
      : id_(mapping.id()),
        create_ts_(create_ts),
        range_(static_cast<uint64_t>(mapping.start()),
               static_cast<uint64_t>(mapping.end())) {}
  bool Contains(uint64_t address) const { return range_.Contains(address); }
  bool Contains(const AddressRange& range) const {
    return range_.Contains(range);
  }
  uint64_t start() const { return range_.start(); }
  uint64_t end() const { return range_.end(); }
  int64_t create_ts() const { return create_ts_; }
  MappingId id() const { return id_; }

  MappingVersion SplitFront(uint64_t mid);

 private:
  MappingVersion(MappingId id, int64_t create_ts, AddressRange alive_range)
      : id_(id), create_ts_(create_ts), range_(alive_range) {}
  MappingId id_;
  int64_t create_ts_;
  AddressRange range_;
};

}  // namespace perfetto::trace_processor::etm

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ETM_MAPPING_VERSION_H_
