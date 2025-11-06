/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/perfetto_cmd/perfetto_cmd.h"

#include <sys/sendfile.h>

#include <unistd.h>
#include <cinttypes>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_mmap.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "src/android_internal/incident_service.h"
#include "src/android_internal/lazy_library_loader.h"
#include "src/android_internal/tracing_service_proxy.h"

#include "protos/perfetto/config/trace_config.gen.h"

#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

// traced runs as 'user nobody' (AID_NOBODY), defined here:
// https://cs.android.com/android/platform/superproject/+/android-latest-release:system/core/libcutils/include/private/android_filesystem_config.h;l=203;drc=f5b540e2b7b9b325d99486d49c0ac57bdd0c5344
// We only trust packages written by traced.
static constexpr int32_t kTrustedUid = 9999;

// Directories for local state and temporary files. These are automatically
// created by the system by setting setprop persist.traced.enable=1.
const char* kStateDir = "/data/misc/perfetto-traces";
const char* kStatePersistentRunningDir =
    "/data/misc/perfetto-traces/persistent/running";
const char* kStatePersistentUploadingDir =
    "/data/misc/perfetto-traces/persistent/uploading";

constexpr int64_t kSendfileTimeoutNs = 10UL * 1000 * 1000 * 1000;  // 10s

}  // namespace

void PerfettoCmd::SaveTraceIntoIncidentOrCrash() {
  PERFETTO_CHECK(save_to_incidentd_);

  const auto& cfg = trace_config_->incident_report_config();
  PERFETTO_CHECK(!cfg.destination_package().empty());
  PERFETTO_CHECK(!cfg.skip_incidentd());

  if (bytes_written_ == 0) {
    LogUploadEvent(PerfettoStatsdAtom::kNotUploadingEmptyTrace);
    PERFETTO_LOG("Skipping write to incident. Empty trace.");
    return;
  }

  // Save the trace as an incident.
  SaveOutputToIncidentTraceOrCrash();

  // Skip the trace-uuid link for traces that are too small. Realistically those
  // traces contain only a marker (e.g. seized_for_bugreport, or the trace
  // expired without triggers). Those are useless and introduce only noise.
  if (bytes_written_ > 4096) {
    base::Uuid uuid(uuid_);
    PERFETTO_LOG("go/trace-uuid/%s name=\"%s\" size=%" PRIu64,
                 uuid.ToPrettyString().c_str(),
                 trace_config_->unique_session_name().c_str(), bytes_written_);
  }

  // Ask incidentd to create a report, which will read the file we just
  // wrote.
  PERFETTO_LAZY_LOAD(android_internal::StartIncidentReport, incident_fn);
  PERFETTO_CHECK(incident_fn(cfg.destination_package().c_str(),
                             cfg.destination_class().c_str(),
                             cfg.privacy_level()));
}

void PerfettoCmd::ReportTraceToAndroidFrameworkOrCrash() {
  PERFETTO_CHECK(report_to_android_framework_);
  PERFETTO_CHECK(trace_out_stream_);

  const auto& cfg = trace_config_->android_report_config();
  PERFETTO_CHECK(!cfg.reporter_service_package().empty());
  PERFETTO_CHECK(!cfg.skip_report());

  if (bytes_written_ == 0) {
    LogUploadEvent(PerfettoStatsdAtom::kCmdFwReportEmptyTrace);
    PERFETTO_LOG("Skipping reporting trace to Android. Empty trace.");
    return;
  }

  LogUploadEvent(PerfettoStatsdAtom::kCmdFwReportBegin);
  base::StackString<128> self_fd("/proc/self/fd/%d",
                                 fileno(*trace_out_stream_));
  base::ScopedFile fd(base::OpenFile(self_fd.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd) {
    PERFETTO_FATAL("Failed to dup fd when reporting to Android");
  }

  base::Uuid uuid(uuid_);
  PERFETTO_LAZY_LOAD(android_internal::ReportTrace, report_fn);
  PERFETTO_CHECK(report_fn(cfg.reporter_service_package().c_str(),
                           cfg.reporter_service_class().c_str(), fd.release(),
                           uuid.lsb(), uuid.msb(),
                           cfg.use_pipe_in_framework_for_testing()));

  // Skip the trace-uuid link for traces that are too small. Realistically those
  // traces contain only a marker (e.g. seized_for_bugreport, or the trace
  // expired without triggers). Those are useless and introduce only noise.
  if (bytes_written_ > 4096) {
    PERFETTO_LOG("go/trace-uuid/%s name=\"%s\" size=%" PRIu64,
                 uuid.ToPrettyString().c_str(),
                 trace_config_->unique_session_name().c_str(), bytes_written_);
  }
  LogUploadEvent(PerfettoStatsdAtom::kCmdFwReportHandoff);
}

// static
void PerfettoCmd::ReportAllPersistentTracesToAndroidFrameworkOrCrash() {
  std::vector<std::string> file_names;
  auto status =
      base::ListFilesRecursive(kStatePersistentUploadingDir, file_names);
  if (!status.ok()) {
    PERFETTO_DLOG("Failed to get the list of persistent traces to upload: %s",
                  status.c_message());
    return;
  }

  std::vector<std::string> file_paths;
  for (const std::string& name : file_names) {
    file_paths.push_back(std::string(kStatePersistentUploadingDir) + "/" +
                         name);
  }

  std::vector<std::pair<base::ScopedFile, TraceConfig>>
      traces_to_upload;
  for (const std::string& path : file_paths) {
    bool is_empty_file = base::GetFileSize(path).value_or(0) == 0;
    if (is_empty_file)
      continue;
    base::ScopedMmap mmaped_file = base::ReadMmapWholeFile(path.c_str());
    if (!mmaped_file.IsValid()) {
      PERFETTO_PLOG("Failed to mmap trace file '%s'", path.c_str());
      continue;
    }
    auto maybe_report_config =
        ParseTraceConfigFromMmapedTrace(std::move(mmaped_file));
    if (maybe_report_config) {
      base::ScopedFile fd = base::OpenFile(path, O_RDONLY | O_CLOEXEC);
      if (!fd) {
        PERFETTO_PLOG("Failed to open trace file '%s' for upload",
                      path.c_str());
        continue;
      }
      traces_to_upload.emplace_back(std::move(fd), *maybe_report_config);
    }
  }

  for (const std::string& path : file_paths) {
    unlink(path.c_str());
  }
}

// Open a staging file (unlinking the previous instance), copy the trace
// contents over, then rename to a final hardcoded path (known to incidentd).
// Such tracing sessions should not normally overlap. We do not use unique
// unique filenames to avoid creating an unbounded amount of files in case of
// errors.
void PerfettoCmd::SaveOutputToIncidentTraceOrCrash() {
  LogUploadEvent(PerfettoStatsdAtom::kUploadIncidentBegin);
  base::StackString<256> kIncidentTracePath("%s/incident-trace", kStateDir);

  base::StackString<256> kTempIncidentTracePath("%s.temp",
                                                kIncidentTracePath.c_str());

  PERFETTO_CHECK(unlink(kTempIncidentTracePath.c_str()) == 0 ||
                 errno == ENOENT);

  // TODO(b/155024256) These should not be necessary (we flush when destroying
  // packet writer and sendfile should ignore file offset) however they should
  // not harm anything and it will help debug the linked issue.
  PERFETTO_CHECK(fflush(*trace_out_stream_) == 0);
  PERFETTO_CHECK(fseek(*trace_out_stream_, 0, SEEK_SET) == 0);

  // SELinux constrains the set of readers.
  base::ScopedFile staging_fd = base::OpenFile(kTempIncidentTracePath.c_str(),
                                               O_CREAT | O_EXCL | O_RDWR, 0666);
  PERFETTO_CHECK(staging_fd);

  int fd = fileno(*trace_out_stream_);
  off_t offset = 0;
  size_t remaining = static_cast<size_t>(bytes_written_);

  // Count time in terms of CPU to avoid timeouts due to suspend:
  base::TimeNanos start = base::GetThreadCPUTimeNs();
  for (;;) {
    errno = 0;
    PERFETTO_DCHECK(static_cast<size_t>(offset) + remaining == bytes_written_);
    auto wsize = PERFETTO_EINTR(sendfile(*staging_fd, fd, &offset, remaining));
    if (wsize < 0) {
      PERFETTO_FATAL("sendfile() failed wsize=%zd, off=%" PRId64
                     ", initial=%" PRIu64 ", remaining=%zu",
                     wsize, static_cast<int64_t>(offset), bytes_written_,
                     remaining);
    }
    remaining -= static_cast<size_t>(wsize);
    if (remaining == 0) {
      break;
    }
    base::TimeNanos now = base::GetThreadCPUTimeNs();
    if (now < start || (now - start).count() > kSendfileTimeoutNs) {
      PERFETTO_FATAL("sendfile() timed out wsize=%zd, off=%" PRId64
                     ", initial=%" PRIu64
                     ", remaining=%zu, start=%lld, now=%lld",
                     wsize, static_cast<int64_t>(offset), bytes_written_,
                     remaining, static_cast<long long int>(start.count()),
                     static_cast<long long int>(now.count()));
    }
  }

  staging_fd.reset();
  PERFETTO_CHECK(
      rename(kTempIncidentTracePath.c_str(), kIncidentTracePath.c_str()) == 0);
  // Note: not calling fsync(2), as we're not interested in the file being
  // consistent in case of a crash.
  LogUploadEvent(PerfettoStatsdAtom::kUploadIncidentSuccess);
}

// static
base::ScopedFile PerfettoCmd::CreateUnlinkedTmpFile() {
  // If we are tracing to DropBox, there's no need to make a
  // filesystem-visible temporary file.
  auto fd = base::OpenFile(kStateDir, O_TMPFILE | O_RDWR, 0600);
  if (!fd)
    PERFETTO_PLOG("Could not create a temporary trace file in %s", kStateDir);
  return fd;
}

// static
base::ScopedFile PerfettoCmd::CreatePersistentTraceFile(
    const std::string& unique_session_name) {
  std::string name =
      unique_session_name.empty() ? "trace" : unique_session_name.substr(0, 64);
  base::StackString<256> file_path("%s/%s.pftrace", kStatePersistentRunningDir,
                                   name.c_str());
  // TODO(ktimofeev): use flock(2) to check if the trace file is currently opend
  // by the traced or just wasn't rm-ed on the reboot. If it wasn't rm-ed
  // overwrite it.
  // we can use base::OpenFile with "O_CREAT | O_EXCL" flags to check if file
  // exists.
  if (base::FileExists(file_path.ToStdString())) {
    PERFETTO_ELOG(
        "Could not create a persistent trace file '%s' for session name: '%s', "
        "file already exists",
        file_path.c_str(), name.c_str());
    return base::ScopedFile{};  // Invalid file.
  }
  auto fd = base::OpenFile(file_path.ToStdString(), O_CREAT | O_RDWR, 0600);
  if (!fd) {
    PERFETTO_PLOG("Could not create a persistent trace file '%s'",
                  file_path.c_str());
  }
  return fd;
}

// static
std::optional<TraceConfig> PerfettoCmd::ParseTraceConfigFromMmapedTrace(
    base::ScopedMmap mmapped_trace) {
  PERFETTO_CHECK(mmapped_trace.IsValid());

  protozero::ProtoDecoder trace_decoder(mmapped_trace.data(),
                                        mmapped_trace.length());

  for (auto packet = trace_decoder.ReadField(); packet;
       packet = trace_decoder.ReadField()) {
    if (packet.id() != protos::pbzero::Trace::kPacketFieldNumber ||
        packet.type() !=
            protozero::proto_utils::ProtoWireType::kLengthDelimited) {
      return std::nullopt;
    }

    protozero::ProtoDecoder packet_decoder(packet.as_bytes());

    auto trace_config_field = packet_decoder.FindField(
        protos::pbzero::TracePacket::kTraceConfigFieldNumber);
    if (!trace_config_field)
      continue;

    auto trusted_uid_field = packet_decoder.FindField(
        protos::pbzero::TracePacket::kTrustedUidFieldNumber);
    if (!trusted_uid_field)
      continue;

    int32_t uid_value = trusted_uid_field.as_int32();

    if (uid_value != kTrustedUid)
      continue;

    TraceConfig trace_config;
    if (trace_config.ParseFromArray(trace_config_field.data(),
                                    trace_config_field.size())) {
      return trace_config;
    }
  }

  return std::nullopt;
}

}  // namespace perfetto
