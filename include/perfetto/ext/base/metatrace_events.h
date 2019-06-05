/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_METATRACE_EVENTS_H_
#define INCLUDE_PERFETTO_EXT_BASE_METATRACE_EVENTS_H_

#include <stdint.h>

namespace perfetto {
namespace metatrace {

enum Tags : uint32_t {
  TAG_NONE = 0,
  TAG_ANY = uint32_t(-1),
  TAG_FTRACE = 1 << 0,
  TAG_PROC_POLLERS = 1 << 1,
};

// Compile time list of parsing and processing stats.
// The macros below generate matching enums and arrays of string literals.
// This is to avoid maintaining string maps manually.

// clang-format off

// DO NOT remove or reshuffle items in this list, only append. The ID of these
// events are an ABI, the trace processor relies on these to open old traces.
#define PERFETTO_METATRACE_EVENTS(F) \
  F(EVENT_ZERO_UNUSED),\
  F(FTRACE_CPU_READER_READ), \
  F(FTRACE_DRAIN_CPUS), \
  F(FTRACE_UNBLOCK_READERS), \
  F(FTRACE_CPU_READ_NONBLOCK), \
  F(FTRACE_CPU_READ_BLOCK), \
  F(FTRACE_CPU_SPLICE_NONBLOCK), \
  F(FTRACE_CPU_SPLICE_BLOCK), \
  F(FTRACE_CPU_WAIT_CMD), \
  F(FTRACE_CPU_RUN_CYCLE), \
  F(FTRACE_CPU_FLUSH), \
  F(FTRACE_CPU_DRAIN), \
  F(READ_SYS_STATS), \
  F(PS_WRITE_ALL_PROCESSES), \
  F(PS_ON_PIDS), \
  F(PS_ON_RENAME_PIDS), \
  F(PS_WRITE_ALL_PROCESS_STATS)

// Append only, see above.
#define PERFETTO_METATRACE_COUNTERS(F) \
  F(COUNTER_ZERO_UNUSED),\
  F(FTRACE_PAGES_DRAINED), \
  F(PS_PIDS_SCANNED)

// clang-format on

#define PERFETTO_METATRACE_IDENTITY(name) name
#define PERFETTO_METATRACE_TOSTRING(name) #name

enum Events : uint16_t {
  PERFETTO_METATRACE_EVENTS(PERFETTO_METATRACE_IDENTITY),
  EVENTS_MAX
};
constexpr char const* kEventNames[] = {
    PERFETTO_METATRACE_EVENTS(PERFETTO_METATRACE_TOSTRING)};

enum Counters : uint16_t {
  PERFETTO_METATRACE_COUNTERS(PERFETTO_METATRACE_IDENTITY),
  COUNTERS_MAX
};
constexpr char const* kCounterNames[] = {
    PERFETTO_METATRACE_COUNTERS(PERFETTO_METATRACE_TOSTRING)};

}  // namespace metatrace
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_METATRACE_EVENTS_H_
