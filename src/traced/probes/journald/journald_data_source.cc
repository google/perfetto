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

#include "src/traced/probes/journald/journald_data_source.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <dlfcn.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/config/linux/journald_config.pbzero.h"
#include "protos/perfetto/trace/linux/journald_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

constexpr uint32_t kMaxEventsPerRead = 500;

// Locally defined to avoid needing <systemd/sd-journal.h> at compile time.
struct sd_id128_t {
  uint8_t bytes[16];
};

// Function pointer typedefs matching the real libsystemd signatures.
using sd_journal_open_t = int (*)(sd_journal**, int);
using sd_journal_close_t = void (*)(sd_journal*);
using sd_journal_add_match_t = int (*)(sd_journal*, const void*, size_t);
using sd_journal_add_disjunction_t = int (*)(sd_journal*);
using sd_journal_add_conjunction_t = int (*)(sd_journal*);
using sd_journal_seek_tail_t = int (*)(sd_journal*);
using sd_journal_previous_t = int (*)(sd_journal*);
using sd_journal_next_t = int (*)(sd_journal*);
using sd_journal_get_fd_t = int (*)(sd_journal*);
using sd_journal_process_t = int (*)(sd_journal*);
using sd_journal_get_monotonic_usec_t = int (*)(sd_journal*,
                                                uint64_t*,
                                                sd_id128_t*);
using sd_journal_get_data_t = int (*)(sd_journal*,
                                      const char*,
                                      const void**,
                                      size_t*);

// Constants from <systemd/sd-journal.h> — defined locally so we don't need
// the systemd headers at compile time.
static constexpr int kSdJournalLocalOnly = 1 << 0;
}  // namespace

struct JournaldDataSource::SdJournalApi {
  sd_journal_open_t open;
  sd_journal_close_t close;
  sd_journal_add_match_t add_match;
  sd_journal_add_disjunction_t add_disjunction;
  sd_journal_add_conjunction_t add_conjunction;
  sd_journal_seek_tail_t seek_tail;
  sd_journal_previous_t previous;
  sd_journal_next_t next;
  sd_journal_get_fd_t get_fd;
  sd_journal_process_t process;
  sd_journal_get_monotonic_usec_t get_monotonic_usec;
  sd_journal_get_data_t get_data;
};

std::unique_ptr<JournaldDataSource::SdJournalApi>
JournaldDataSource::LoadSdJournalApi() {
  const char* libsystemd_soname = "libsystemd.so.0";
  void* handle = dlopen(libsystemd_soname, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    PERFETTO_ELOG(
        "linux.systemd_journald datasource unavailable, failed to load %s: %s",
        libsystemd_soname, dlerror());
    return nullptr;
  }

  auto api = std::make_unique<JournaldDataSource::SdJournalApi>();
  auto load_sym = [&](const char* name) -> void* {
    void* sym = dlsym(handle, name);
    if (!sym)
      PERFETTO_ELOG("dlsym(\"%s\", %s) failed: %s", libsystemd_soname, name,
                    dlerror());
    return sym;
  };

  api->open = reinterpret_cast<sd_journal_open_t>(load_sym("sd_journal_open"));
  if (!api->open)
    return nullptr;

  api->close =
      reinterpret_cast<sd_journal_close_t>(load_sym("sd_journal_close"));
  if (!api->close)
    return nullptr;

  api->add_match = reinterpret_cast<sd_journal_add_match_t>(
      load_sym("sd_journal_add_match"));
  if (!api->add_match)
    return nullptr;

  api->add_disjunction = reinterpret_cast<sd_journal_add_disjunction_t>(
      load_sym("sd_journal_add_disjunction"));
  if (!api->add_disjunction)
    return nullptr;

  api->add_conjunction = reinterpret_cast<sd_journal_add_conjunction_t>(
      load_sym("sd_journal_add_conjunction"));
  if (!api->add_conjunction)
    return nullptr;

  api->seek_tail = reinterpret_cast<sd_journal_seek_tail_t>(
      load_sym("sd_journal_seek_tail"));
  if (!api->seek_tail)
    return nullptr;

  api->previous =
      reinterpret_cast<sd_journal_previous_t>(load_sym("sd_journal_previous"));
  if (!api->previous)
    return nullptr;

  api->next = reinterpret_cast<sd_journal_next_t>(load_sym("sd_journal_next"));
  if (!api->next)
    return nullptr;

  api->get_fd =
      reinterpret_cast<sd_journal_get_fd_t>(load_sym("sd_journal_get_fd"));
  if (!api->get_fd)
    return nullptr;

  api->process =
      reinterpret_cast<sd_journal_process_t>(load_sym("sd_journal_process"));
  if (!api->process)
    return nullptr;

  api->get_monotonic_usec = reinterpret_cast<sd_journal_get_monotonic_usec_t>(
      load_sym("sd_journal_get_monotonic_usec"));
  if (!api->get_monotonic_usec)
    return nullptr;

  api->get_data =
      reinterpret_cast<sd_journal_get_data_t>(load_sym("sd_journal_get_data"));
  if (!api->get_data)
    return nullptr;

  return api;
};

const ProbesDataSource::Descriptor JournaldDataSource::descriptor = {
    /*name*/ "linux.systemd_journald",
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
  protos::pbzero::SystemdJournaldConfig::Decoder cfg(
      ds_config.journald_config_raw());
  if (cfg.has_min_prio())
    min_prio_ = cfg.min_prio();
  for (auto id = cfg.filter_identifiers(); id; ++id)
    filter_identifiers_.push_back(id->as_std_string());
  for (auto u = cfg.filter_units(); u; ++u)
    filter_units_.push_back(u->as_std_string());
}

JournaldDataSource::~JournaldDataSource() {
  if (journal_ && sd_ && fd_ != -1) {
    task_runner_->RemoveFileDescriptorWatch(fd_);
    sd_->close(journal_);
    journal_ = nullptr;
  }
}

// Helper macro to check libsystemd return codes and raise up any errors
#define SD_CHECK(ex)                                       \
  do {                                                     \
    int sd_res = (ex);                                     \
    if (sd_res < 0) {                                      \
      PERFETTO_ELOG(#ex " failed: %s", strerror(-sd_res)); \
      return;                                              \
    }                                                      \
  } while (false)

void JournaldDataSource::Start() {
  sd_ = LoadSdJournalApi();
  if (!sd_) {
    PERFETTO_ELOG("Failed to load libsystemd dynamically; journald disabled.");
    return;
  }

  int r = sd_->open(&journal_, kSdJournalLocalOnly);
  if (r < 0) {
    PERFETTO_ELOG("Failed to open journal: %d", -r);
    return;
  }

  // Add PRIORITY match filters. For each severity level <= min_prio_,
  // add a match with OR (disjunction) logic between levels.
  for (uint32_t p = 0; p <= min_prio_; ++p) {
    std::string match = "PRIORITY=" + std::to_string(p);
    SD_CHECK(sd_->add_match(journal_, match.c_str(), match.size()));
    if (p < min_prio_)
      SD_CHECK(sd_->add_disjunction(journal_));
  }

  // If identifier filters: add them conjuncted with the priority block.
  if (!filter_identifiers_.empty()) {
    SD_CHECK(sd_->add_conjunction(journal_));
    for (size_t i = 0; i < filter_identifiers_.size(); ++i) {
      std::string match = "SYSLOG_IDENTIFIER=" + filter_identifiers_[i];
      SD_CHECK(sd_->add_match(journal_, match.c_str(), match.size()));
      if (i + 1 < filter_identifiers_.size())
        SD_CHECK(sd_->add_disjunction(journal_));
    }
  }

  // Unit filters similarly conjuncted.
  if (!filter_units_.empty()) {
    SD_CHECK(sd_->add_conjunction(journal_));
    for (size_t i = 0; i < filter_units_.size(); ++i) {
      std::string match = "_SYSTEMD_UNIT=" + filter_units_[i];
      SD_CHECK(sd_->add_match(journal_, match.c_str(), match.size()));
      if (i + 1 < filter_units_.size())
        SD_CHECK(sd_->add_disjunction(journal_));
    }
  }

  // Seek to tail so only new entries are captured going forward.
  SD_CHECK(sd_->seek_tail(journal_));
  SD_CHECK(sd_->previous(journal_));

  fd_ = sd_->get_fd(journal_);
  if (fd_ < 0) {
    PERFETTO_ELOG("sd_journal_get_fd failed: %d", -fd_);
    sd_->close(journal_);
    journal_ = nullptr;
    return;
  }

  // Register the fd watch
  auto weak = weak_factory_.GetWeakPtr();
  task_runner_->AddFileDescriptorWatch(fd_, [weak] {
    if (weak)
      weak->OnJournalReadable();
  });
}

void JournaldDataSource::OnJournalReadable() {
  SD_CHECK(sd_->process(journal_));
  ReadJournalEntries();
}

void JournaldDataSource::ReadJournalEntries() {
  uint32_t n = 0;

  while (sd_->next(journal_) > 0 && n < kMaxEventsPerRead) {
    std::string prio_str = GetField("PRIORITY");
    uint32_t prio = prio_str.empty() ? min_prio_
                                     : static_cast<uint32_t>(std::strtoul(
                                           prio_str.c_str(), nullptr, 10));
    uint64_t monotonic_us = 0;
    if (sd_->get_monotonic_usec(journal_, &monotonic_us, NULL) < 0) {
      PERFETTO_LOG("failed to get monotonic timestamp for journald event");
      stats_.num_failed++;
      continue;
    }

    auto packet = writer_->NewTracePacket();
    packet->set_timestamp(monotonic_us * 1000);
    packet->set_timestamp_clock_id(protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
    auto* ev = packet->set_journald_event();
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
}

std::string JournaldDataSource::GetField(const char* field) {
  const void* data = nullptr;
  size_t len = 0;
  if (sd_->get_data(journal_, field, &data, &len) < 0)
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
  if (journal_ && sd_) {
    OnJournalReadable();
  }

  // Emit a stats packet.
  {
    auto packet = writer_->NewTracePacket();
    packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
    packet->set_timestamp_clock_id(protos::pbzero::BUILTIN_CLOCK_BOOTTIME);
    auto* stats = packet->set_journald_event();
    stats->set_num_total(stats_.num_total);
    stats->set_num_failed(stats_.num_failed);
  }

  writer_->Flush(callback);
}

}  // namespace perfetto
