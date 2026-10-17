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

#include "src/traced/probes/cgroup/cgroup_data_source.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "protos/perfetto/config/linux/cgroup_config.pbzero.h"
#include "protos/perfetto/trace/linux/memcg_stats.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

// static
const ProbesDataSource::Descriptor CgroupDataSource::descriptor = {
    "linux.cgroup",
    Descriptor::kFlagsNone,
    nullptr,
};

CgroupDataSource::CgroupDataSource(const DataSourceConfig& ds_config,
                                   base::TaskRunner* task_runner,
                                   TracingSessionID session_id,
                                   std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, &descriptor),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      weak_factory_(this) {
  protos::pbzero::CgroupConfig::Decoder config(ds_config.cgroup_config_raw());
  if (!config.has_memcg_config()) {
    PERFETTO_ELOG("linux.cgroup: no memcg_config, nothing to record");
    return;
  }
  std::optional<MemcgReader::Config> memcg_config =
      MemcgReader::Config::Create(config.memcg_config());
  if (!memcg_config.has_value()) {
    PERFETTO_ELOG("linux.cgroup: invalid memcg_config, nothing to record");
    return;
  }
  memcg_reader_.emplace(std::move(*memcg_config));
}

CgroupDataSource::~CgroupDataSource() = default;

void CgroupDataSource::Start() {
  if (memcg_reader_.has_value()) {
    Tick();
  }
}

void CgroupDataSource::Tick() {
  // Use a single timestamp per tick for simplicity.
  auto start_ts = static_cast<uint64_t>(base::GetBootTimeNs().count());

  auto packet = writer_->NewTracePacket();
  packet->set_timestamp(start_ts);
  auto* stats = packet->set_memcg_stats();
  memcg_reader_->Read(stats);

  // Repost if periodic.
  uint32_t period_ms = memcg_reader_->config().poll_period_ms;
  if (period_ms > 0) {
    auto now_ms = static_cast<uint64_t>(base::GetWallTimeMs().count());
    auto delay_ms = static_cast<uint32_t>(period_ms - (now_ms % period_ms));
    auto weak_this = weak_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(
        [weak_this] {
          if (weak_this)
            weak_this->Tick();
        },
        delay_ms);
  }
}

void CgroupDataSource::Flush(FlushRequestID, std::function<void()> callback) {
  writer_->Flush(std::move(callback));
}

}  // namespace perfetto
