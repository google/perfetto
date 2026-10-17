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

#ifndef SRC_TOOLS_TRACE_REPLAY_REPLAY_FILE_H_
#define SRC_TOOLS_TRACE_REPLAY_REPLAY_FILE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/base/status.h"

namespace perfetto {
namespace trace_replay {

// One TracePacket to be replayed. `bytes` is the *inner* serialized form of the
// original TracePacket (i.e. the bytes that were the value of Trace.packet=1 in
// the input trace). The producer writes them back via
// TracePacket::AppendRawProtoBytes() inside a freshly opened packet.
struct ReplayRecord {
  uint64_t rel_ts_ns = 0;
  uint32_t orig_seq_id = 0;
  uint32_t buffer_idx = 0;
  std::vector<uint8_t> bytes;
};

// A whole replay file is `Header` + N records as below. Little-endian, packed.
//   magic[8]  = "PREPLAY1"
//   num_recs  : uint32
//   num_buffers : uint32   (the total buffer count of the forged config; this
//                           tells the worker how many DSDs to register.)
// Records (each):
//   rel_ts_ns    : uint64
//   orig_seq_id  : uint32
//   buffer_idx   : uint32
//   payload_size : uint32
//   payload[]    : bytes
struct ReplayFileHeader {
  char magic[8];  // "PREPLAY1"
  uint32_t num_records;
  uint32_t num_buffers;
};
static_assert(sizeof(ReplayFileHeader) == 16, "ReplayFileHeader packed");

// Writes the replay records sorted by rel_ts_ns ascending to `out_path`.
base::Status WriteReplayFile(const std::string& out_path,
                             uint32_t num_buffers,
                             const std::vector<ReplayRecord>& records);

// Reads a replay file in full into memory.
base::Status ReadReplayFile(const std::string& path,
                            uint32_t* num_buffers,
                            std::vector<ReplayRecord>* records);

}  // namespace trace_replay
}  // namespace perfetto

#endif  // SRC_TOOLS_TRACE_REPLAY_REPLAY_FILE_H_
