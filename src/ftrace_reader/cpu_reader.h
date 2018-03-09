/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_FTRACE_READER_CPU_READER_H_
#define SRC_FTRACE_READER_CPU_READER_H_

#include <stdint.h>
#include <string.h>

#include <array>
#include <memory>
#include <set>
#include <thread>

#include "gtest/gtest_prod.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/thread_checker.h"
#include "perfetto/ftrace_reader/ftrace_controller.h"
#include "perfetto/protozero/message.h"
#include "proto_translation_table.h"

namespace perfetto {

class ProtoTranslationTable;

namespace protos {
namespace pbzero {
class FtraceEventBundle;
}  // namespace pbzero
}  // namespace protos

// Class for efficient 'is event with id x enabled?' tests.
// Mirrors the data in a FtraceConfig but in a format better suited
// to be consumed by CpuReader.
class EventFilter {
 public:
  EventFilter(const ProtoTranslationTable&, std::set<std::string>);
  ~EventFilter();

  bool IsEventEnabled(size_t ftrace_event_id) const {
    if (ftrace_event_id == 0 || ftrace_event_id > enabled_ids_.size()) {
      return false;
    }
    return enabled_ids_[ftrace_event_id];
  }

  const std::set<std::string>& enabled_names() const { return enabled_names_; }

 private:
  EventFilter(const EventFilter&) = delete;
  EventFilter& operator=(const EventFilter&) = delete;

  const std::vector<bool> enabled_ids_;
  std::set<std::string> enabled_names_;
};

// Processes raw ftrace data for a logical CPU core.
class CpuReader {
 public:
  // |on_data_available| will be called on an arbitrary thread when at least one
  // page of ftrace data is available for draining on this CPU.
  CpuReader(const ProtoTranslationTable*,
            size_t cpu,
            base::ScopedFile fd,
            std::function<void()> on_data_available);
  ~CpuReader();

  // Drains all available data from the staging pipe into the given sinks.
  // Should be called in response to the |on_data_available| callback.
  bool Drain(const std::array<const EventFilter*, kMaxSinks>&,
             const std::array<
                 protozero::MessageHandle<protos::pbzero::FtraceEventBundle>,
                 kMaxSinks>&,
             const std::array<FtraceMetadata*, kMaxSinks>& metadatas);

  template <typename T>
  static bool ReadAndAdvance(const uint8_t** ptr, const uint8_t* end, T* out) {
    if (*ptr > end - sizeof(T))
      return false;
    memcpy(reinterpret_cast<void*>(out), reinterpret_cast<const void*>(*ptr),
           sizeof(T));
    *ptr += sizeof(T);
    return true;
  }

  // Caller must do the bounds check:
  // [start + offset, start + offset + sizeof(T))
  // Returns the raw value not the varint.
  template <typename T>
  static T ReadIntoVarInt(const uint8_t* start,
                          size_t field_id,
                          protozero::Message* out) {
    T t;
    memcpy(&t, reinterpret_cast<const void*>(start), sizeof(T));
    out->AppendVarInt<T>(field_id, t);
    return t;
  }

  template <typename T>
  static void ReadInode(const uint8_t* start,
                        size_t field_id,
                        protozero::Message* out,
                        FtraceMetadata* metadata) {
    T t = ReadIntoVarInt<T>(start, field_id, out);
    metadata->AddInode(t);
  }

  static void ReadDevId(const uint8_t* start,
                        size_t field_id,
                        protozero::Message* out,
                        FtraceMetadata* metadata) {
    uint32_t dev_id = ReadIntoVarInt<uint32_t>(start, field_id, out);
    PERFETTO_DCHECK(dev_id != 0);
    metadata->AddDevice(dev_id);
  }

  static void ReadPid(const uint8_t* start,
                      size_t field_id,
                      protozero::Message* out,
                      FtraceMetadata* metadata) {
    int32_t pid = ReadIntoVarInt<int32_t>(start, field_id, out);
    metadata->AddPid(pid);
  }

  // Iterate through every file in the current directory and check if the inode
  // number of each file matches any of the inode numbers saved in events.
  // Returns map of inode number to filename for every inode number that is
  // found in the filesystem. If the inode number saved from events is not
  // found, nothing is added to the map.
  static std::map<uint64_t, std::string> GetFilenamesForInodeNumbers(
      const std::set<uint64_t>& inode_numbers);

  // Parse a raw ftrace page beginning at ptr and write the events a protos
  // into the provided bundle respecting the given event filter.
  // |table| contains the mix of compile time (e.g. proto field ids) and
  // run time (e.g. field offset and size) information necessary to do this.
  // The table is initialized once at start time by the ftrace controller
  // which passes it to the CpuReader which passes it here.
  static size_t ParsePage(const uint8_t* ptr,
                          const EventFilter*,
                          protos::pbzero::FtraceEventBundle*,
                          const ProtoTranslationTable* table,
                          FtraceMetadata*);

  // Parse a single raw ftrace event beginning at |start| and ending at |end|
  // and write it into the provided bundle as a proto.
  // |table| contains the mix of compile time (e.g. proto field ids) and
  // run time (e.g. field offset and size) information necessary to do this.
  // The table is initialized once at start time by the ftrace controller
  // which passes it to the CpuReader which passes it to ParsePage which
  // passes it here.
  static bool ParseEvent(uint16_t ftrace_event_id,
                         const uint8_t* start,
                         const uint8_t* end,
                         const ProtoTranslationTable* table,
                         protozero::Message* message,
                         FtraceMetadata* metadata);

  static bool ParseField(const Field& field,
                         const uint8_t* start,
                         const uint8_t* end,
                         protozero::Message* message,
                         FtraceMetadata* metadata);

 private:
  static void RunWorkerThread(size_t cpu,
                              int trace_fd,
                              int staging_write_fd,
                              const std::function<void()>& on_data_available);

  uint8_t* GetBuffer();
  CpuReader(const CpuReader&) = delete;
  CpuReader& operator=(const CpuReader&) = delete;

  const ProtoTranslationTable* table_;
  const size_t cpu_;
  base::ScopedFile trace_fd_;
  base::ScopedFile staging_read_fd_;
  base::ScopedFile staging_write_fd_;
  std::unique_ptr<uint8_t[]> buffer_;
  std::thread worker_thread_;
  PERFETTO_THREAD_CHECKER(thread_checker_)
};

}  // namespace perfetto

#endif  // SRC_FTRACE_READER_CPU_READER_H_
