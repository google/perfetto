/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/traced/probes/journald/journald_data_source.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <systemd/sd-journal.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/config/linux/journald_config.pbzero.h"
#include "protos/perfetto/trace/linux/journald_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

namespace {
constexpr uint32_t kMaxEventsPerRead = 500;
}  // namespace

// static
const ProbesDataSource::Descriptor JournaldDataSource::descriptor = {
    /*name*/ "linux.journald",
    /*flags*/ Descriptor::kFlagsNone,
    /*fill_descriptor_func*/ nullptr,
};

JournaldDataSource::JournaldDataSource(DataSourceConfig ds_config,
                                       base::TaskRunner* task_runner,
                                       TracingSessionID session_id,
                                       std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, &descriptor),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      weak_factory_(this) {
  protos::pbzero::JournaldConfig::Decoder cfg(ds_config.journald_config_raw());
  if (cfg.has_min_prio())
    min_prio_ = cfg.min_prio();
  for (auto id = cfg.filter_identifiers(); id; ++id)
    filter_identifiers_.push_back(id->as_std_string());
  for (auto u = cfg.filter_units(); u; ++u)
    filter_units_.push_back(u->as_std_string());
}

JournaldDataSource::~JournaldDataSource() {
  if (journal_) {
    task_runner_->RemoveFileDescriptorWatch(sd_journal_get_fd(journal_));
    sd_journal_close(journal_);
    journal_ = nullptr;
  }
}

void JournaldDataSource::Start() {
  int r = sd_journal_open(&journal_, SD_JOURNAL_LOCAL_ONLY);
  if (r < 0) {
    PERFETTO_ELOG("Failed to open journal: %s (errno %d)", strerror(-r), -r);
    return;
  }

  // Add PRIORITY match filters. For each severity level <= min_prio_,
  // add a match with OR (disjunction) logic between levels.
  for (uint32_t p = 0; p <= min_prio_; ++p) {
    std::string match = "PRIORITY=" + std::to_string(p);
    sd_journal_add_match(journal_, match.c_str(), match.size());
    if (p < min_prio_)
      sd_journal_add_disjunction(journal_);
  }

  // If identifier filters: add them conjuncted with the priority block.
  if (!filter_identifiers_.empty()) {
    sd_journal_add_conjunction(journal_);
    for (size_t i = 0; i < filter_identifiers_.size(); ++i) {
      std::string match = "SYSLOG_IDENTIFIER=" + filter_identifiers_[i];
      sd_journal_add_match(journal_, match.c_str(), match.size());
      if (i + 1 < filter_identifiers_.size())
        sd_journal_add_disjunction(journal_);
    }
  }

  // Unit filters similarly conjuncted.
  if (!filter_units_.empty()) {
    sd_journal_add_conjunction(journal_);
    for (size_t i = 0; i < filter_units_.size(); ++i) {
      std::string match = "_SYSTEMD_UNIT=" + filter_units_[i];
      sd_journal_add_match(journal_, match.c_str(), match.size());
      if (i + 1 < filter_units_.size())
        sd_journal_add_disjunction(journal_);
    }
  }

  // Seek to tail so only new entries are captured going forward.
  sd_journal_seek_tail(journal_);
  sd_journal_previous(journal_);

  int fd = sd_journal_get_fd(journal_);
  if (fd < 0) {
    PERFETTO_ELOG("sd_journal_get_fd failed: %s (errno %d)", strerror(-fd),
                  -fd);
    sd_journal_close(journal_);
    journal_ = nullptr;
    return;
  }

  // Register the fd watch before draining so no wakeups are missed.
  auto weak = weak_factory_.GetWeakPtr();
  task_runner_->AddFileDescriptorWatch(fd, [weak] {
    if (weak)
      weak->OnJournalReadable();
  });

  // Drain once after seek_tail(). sd_journal_next() returns 0 immediately
  // (no pre-existing entries are delivered), but this call is required to
  // establish the per-file n_entries baseline in the sd_journal internals.
  // Without it the EOF short-circuit in sd_journal_next() never clears on
  // subsequent wakeups, so new entries written after Start() would never
  // be seen.
  while (sd_journal_next(journal_) > 0) {
  }
}

void JournaldDataSource::OnJournalReadable() {
  sd_journal_process(journal_);
  ReadJournalEntries();
}

void JournaldDataSource::ReadJournalEntries() {
  uint32_t n = 0;
  auto packet = writer_->NewTracePacket();
  packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
  auto* log_packet = packet->set_journald_event();

  while (sd_journal_next(journal_) > 0 && n < kMaxEventsPerRead) {
    std::string prio_str = GetField("PRIORITY");
    uint32_t prio = prio_str.empty() ? min_prio_
                                     : static_cast<uint32_t>(std::strtoul(
                                           prio_str.c_str(), nullptr, 10));
    if (prio > min_prio_) {
      stats_.num_skipped++;
      continue;
    }

    uint64_t realtime_us = 0;
    uint64_t monotonic_us = 0;
    sd_id128_t boot_id{};
    sd_journal_get_realtime_usec(journal_, &realtime_us);
    sd_journal_get_monotonic_usec(journal_, &monotonic_us, &boot_id);

    auto* ev = log_packet->add_events();
    ev->set_timestamp_us(realtime_us);
    ev->set_monotonic_ts_us(monotonic_us);
    ev->set_prio(prio);

    std::string msg = GetField("MESSAGE");
    if (!msg.empty())
      ev->set_message(msg);

    std::string tag = GetField("SYSLOG_IDENTIFIER");
    if (!tag.empty())
      ev->set_tag(tag);

    std::string comm = GetField("_COMM");
    if (!comm.empty())
      ev->set_comm(comm);

    std::string exe = GetField("_EXE");
    if (!exe.empty())
      ev->set_exe(exe);

    std::string unit = GetField("_SYSTEMD_UNIT");
    if (!unit.empty())
      ev->set_systemd_unit(unit);

    std::string host = GetField("_HOSTNAME");
    if (!host.empty())
      ev->set_hostname(host);

    std::string transport = GetField("_TRANSPORT");
    if (!transport.empty())
      ev->set_transport(transport);

    std::string pid_str = GetField("_PID");
    if (!pid_str.empty())
      ev->set_pid(
          static_cast<uint32_t>(std::strtoul(pid_str.c_str(), nullptr, 10)));

    std::string tid_str = GetField("_TID");
    if (!tid_str.empty())
      ev->set_tid(
          static_cast<uint32_t>(std::strtoul(tid_str.c_str(), nullptr, 10)));

    std::string uid_str = GetField("_UID");
    if (!uid_str.empty())
      ev->set_uid(
          static_cast<uint32_t>(std::strtoul(uid_str.c_str(), nullptr, 10)));

    std::string gid_str = GetField("_GID");
    if (!gid_str.empty())
      ev->set_gid(
          static_cast<uint32_t>(std::strtoul(gid_str.c_str(), nullptr, 10)));

    stats_.num_total++;
    n++;
  }

  if (n == 0) {
    // No new events; discard the empty packet.
    packet->Finalize();
  }
}

std::string JournaldDataSource::GetField(const char* field) {
  const void* data = nullptr;
  size_t len = 0;
  if (sd_journal_get_data(journal_, field, &data, &len) < 0)
    return {};
  // sd_journal_get_data returns "FIELD=value"; skip past the '='.
  const char* str = static_cast<const char*>(data);
  const char* eq = static_cast<const char*>(memchr(str, '=', len));
  if (!eq)
    return {};
  ++eq;  // skip '='
  return std::string(eq, static_cast<size_t>(str + len - eq));
}

void JournaldDataSource::Flush(FlushRequestID, std::function<void()> callback) {
  if (journal_) {
    sd_journal_process(journal_);
    ReadJournalEntries();
  }

  // Emit a stats packet.
  {
    auto packet = writer_->NewTracePacket();
    packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
    auto* stats = packet->set_journald_event()->set_stats();
    stats->set_num_total(stats_.num_total);
    stats->set_num_skipped(stats_.num_skipped);
    stats->set_num_failed(stats_.num_failed);
  }

  writer_->Flush(callback);
}

}  // namespace perfetto
