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

#include "src/tools/trace_replay/replay_file.h"

#include <fcntl.h>
#include <string.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"

namespace perfetto {
namespace trace_replay {

namespace {
constexpr char kMagic[8] = {'P', 'R', 'E', 'P', 'L', 'A', 'Y', '1'};
}  // namespace

base::Status WriteReplayFile(const std::string& out_path,
                             uint32_t num_buffers,
                             const std::vector<ReplayRecord>& records_in) {
  // Sort by rel_ts_ns ascending. Stable so equal timestamps keep input order.
  std::vector<ReplayRecord> records = records_in;
  std::stable_sort(records.begin(), records.end(),
                   [](const ReplayRecord& a, const ReplayRecord& b) {
                     return a.rel_ts_ns < b.rel_ts_ns;
                   });

  base::ScopedFile fd(
      base::OpenFile(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644));
  if (!fd)
    return base::ErrStatus("Cannot open %s for writing", out_path.c_str());

  ReplayFileHeader hdr{};
  memcpy(hdr.magic, kMagic, sizeof(hdr.magic));
  hdr.num_records = static_cast<uint32_t>(records.size());
  hdr.num_buffers = num_buffers;

  auto write_blob = [&](const void* data, size_t size) -> bool {
    return base::WriteAll(fd.get(), data, size) == static_cast<ssize_t>(size);
  };

  if (!write_blob(&hdr, sizeof(hdr)))
    return base::ErrStatus("Short write of header to %s", out_path.c_str());

  for (const auto& r : records) {
    uint64_t ts = r.rel_ts_ns;
    uint32_t seq = r.orig_seq_id;
    uint32_t buf = r.buffer_idx;
    uint32_t sz = static_cast<uint32_t>(r.bytes.size());
    if (!write_blob(&ts, sizeof(ts)) || !write_blob(&seq, sizeof(seq)) ||
        !write_blob(&buf, sizeof(buf)) || !write_blob(&sz, sizeof(sz)) ||
        (sz && !write_blob(r.bytes.data(), sz))) {
      return base::ErrStatus("Short write to %s", out_path.c_str());
    }
  }
  return base::OkStatus();
}

base::Status ReadReplayFile(const std::string& path,
                            uint32_t* num_buffers,
                            std::vector<ReplayRecord>* records) {
  std::string blob;
  if (!base::ReadFile(path, &blob))
    return base::ErrStatus("Cannot read %s", path.c_str());

  if (blob.size() < sizeof(ReplayFileHeader))
    return base::ErrStatus("%s is shorter than the header", path.c_str());

  ReplayFileHeader hdr{};
  memcpy(&hdr, blob.data(), sizeof(hdr));
  if (memcmp(hdr.magic, kMagic, 8) != 0)
    return base::ErrStatus("%s: bad magic", path.c_str());

  *num_buffers = hdr.num_buffers;
  records->clear();
  records->reserve(hdr.num_records);

  size_t off = sizeof(hdr);
  for (uint32_t i = 0; i < hdr.num_records; i++) {
    constexpr size_t kRecHdr = sizeof(uint64_t) + sizeof(uint32_t) * 3;
    if (off + kRecHdr > blob.size())
      return base::ErrStatus("%s: truncated record header @%zu", path.c_str(),
                             off);
    uint64_t ts;
    uint32_t seq, buf, sz;
    memcpy(&ts, blob.data() + off, sizeof(ts));
    off += sizeof(ts);
    memcpy(&seq, blob.data() + off, sizeof(seq));
    off += sizeof(seq);
    memcpy(&buf, blob.data() + off, sizeof(buf));
    off += sizeof(buf);
    memcpy(&sz, blob.data() + off, sizeof(sz));
    off += sizeof(sz);
    if (off + sz > blob.size())
      return base::ErrStatus("%s: truncated record payload @%zu", path.c_str(),
                             off);
    ReplayRecord r;
    r.rel_ts_ns = ts;
    r.orig_seq_id = seq;
    r.buffer_idx = buf;
    r.bytes.assign(reinterpret_cast<const uint8_t*>(blob.data() + off),
                   reinterpret_cast<const uint8_t*>(blob.data() + off + sz));
    off += sz;
    records->push_back(std::move(r));
  }
  return base::OkStatus();
}

}  // namespace trace_replay
}  // namespace perfetto
