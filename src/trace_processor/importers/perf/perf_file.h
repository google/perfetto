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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_FILE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_FILE_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "src/trace_processor/importers/perf/perf_event.h"
#include "src/trace_processor/importers/perf/reader.h"

namespace perfetto::trace_processor::perf_importer {

struct PerfFile {
  static constexpr char kMagicLE[] = {'P', 'E', 'R', 'F', 'I', 'L', 'E', '2'};
  static constexpr char kMagicBE[] = {'2', 'E', 'L', 'I', 'F', 'R', 'E', 'P'};

  // Returns the byte order of perf.data starting with |data|, if valid.
  static std::optional<ByteOrder> ByteOrderFromMagic(const uint8_t* data,
                                                     size_t size) {
    if (size < sizeof(kMagicLE)) {
      return std::nullopt;
    }
    if (memcmp(data, kMagicLE, sizeof(kMagicLE)) == 0) {
      return ByteOrder::kLittleEndian;
    }
    if (memcmp(data, kMagicBE, sizeof(kMagicBE)) == 0) {
      return ByteOrder::kBigEndian;
    }
    return std::nullopt;
  }

  struct Section {
    uint64_t offset;
    uint64_t size;
    uint64_t end() const { return offset + size; }
  };

  struct AttrsEntry {
    perf_event_attr attr;
    Section ids;
  };

  struct Header {
    char magic[8];
    uint64_t size;
    // Size of PerfFileAttr struct and section pointing to ids.
    uint64_t attr_size;
    Section attrs;
    Section data;
    Section event_types;
    uint64_t flags;
    uint64_t flags1[3];

    uint64_t num_attrs() const { return attrs.size / attr_size; }
  };
};

inline PerfFile::Section ByteSwap(PerfFile::Section v) {
  return {ByteSwap(v.offset), ByteSwap(v.size)};
}

inline PerfFile::Header ByteSwap(PerfFile::Header v) {
  v.size = ByteSwap(v.size);
  v.attr_size = ByteSwap(v.attr_size);
  v.attrs = ByteSwap(v.attrs);
  v.data = ByteSwap(v.data);
  v.event_types = ByteSwap(v.event_types);
  v.flags = ByteSwap(v.flags);
  for (uint64_t& f : v.flags1) {
    f = ByteSwap(f);
  }
  return v;
}

}  // namespace perfetto::trace_processor::perf_importer

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_PERF_FILE_H_
