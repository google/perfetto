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

#include <inttypes.h>
#include <sys/sendfile.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/tracing/core/trace_config.h"
#include "src/android_internal/incident_service.h"
#include "src/android_internal/lazy_library_loader.h"
#include "src/android_internal/statsd_logging.h"

namespace perfetto {
namespace {

constexpr int64_t kSendfileTimeoutNs = 10UL * 1000 * 1000 * 1000;  // 10s

}  // namespace

void PerfettoCmd::SaveTraceIntoDropboxAndIncidentOrCrash() {
  PERFETTO_CHECK(is_uploading_);
  PERFETTO_CHECK(
      !trace_config_->incident_report_config().destination_package().empty());

  if (bytes_written_ == 0) {
    LogUploadEvent(PerfettoStatsdAtom::kNotUploadingEmptyTrace);
    PERFETTO_LOG("Skipping write to incident. Empty trace.");
    return;
  }

  // Save the trace as an incident.
  SaveOutputToIncidentTraceOrCrash();

  // Ask incidentd to create a report, which will read the file we just
  // wrote.
  const auto& cfg = trace_config_->incident_report_config();
  PERFETTO_LAZY_LOAD(android_internal::StartIncidentReport, incident_fn);
  PERFETTO_CHECK(incident_fn(cfg.destination_package().c_str(),
                             cfg.destination_class().c_str(),
                             cfg.privacy_level()));
}

// Open a staging file (unlinking the previous instance), copy the trace
// contents over, then rename to a final hardcoded path (known to incidentd).
// Such tracing sessions should not normally overlap. We do not use unique
// unique filenames to avoid creating an unbounded amount of files in case of
// errors.
void PerfettoCmd::SaveOutputToIncidentTraceOrCrash() {
  LogUploadEvent(PerfettoStatsdAtom::kUploadIncidentBegin);
  char kIncidentTracePath[256];
  sprintf(kIncidentTracePath, "%s/incident-trace", kStateDir);

  char kTempIncidentTracePath[256];
  sprintf(kTempIncidentTracePath, "%s.temp", kIncidentTracePath);

  PERFETTO_CHECK(unlink(kTempIncidentTracePath) == 0 || errno == ENOENT);

  // TODO(b/155024256) These should not be necessary (we flush when destroying
  // packet writer and sendfile should ignore file offset) however they should
  // not harm anything and it will help debug the linked issue.
  PERFETTO_CHECK(fflush(*trace_out_stream_) == 0);
  PERFETTO_CHECK(fseek(*trace_out_stream_, 0, SEEK_SET) == 0);

  // SELinux constrains the set of readers.
  base::ScopedFile staging_fd =
      base::OpenFile(kTempIncidentTracePath, O_CREAT | O_EXCL | O_RDWR, 0666);
  PERFETTO_CHECK(staging_fd);

  int fd = fileno(*trace_out_stream_);
  off_t offset = 0;
  size_t remaining = static_cast<size_t>(bytes_written_);

  base::TimeNanos start = base::GetBootTimeNs();
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
    if ((base::GetBootTimeNs() - start).count() > kSendfileTimeoutNs) {
      PERFETTO_FATAL("sendfile() timed out wsize=%zd, off=%" PRId64
                     ", initial=%" PRIu64 ", remaining=%zu",
                     wsize, static_cast<int64_t>(offset), bytes_written_,
                     remaining);
    }
  }

  staging_fd.reset();
  PERFETTO_CHECK(rename(kTempIncidentTracePath, kIncidentTracePath) == 0);
  // Note: not calling fsync(2), as we're not interested in the file being
  // consistent in case of a crash.
  LogUploadEvent(PerfettoStatsdAtom::kUploadIncidentSuccess);
}

// static
base::ScopedFile PerfettoCmd::OpenDropboxTmpFile() {
  // If we are tracing to DropBox, there's no need to make a
  // filesystem-visible temporary file.
  auto fd = base::OpenFile(kStateDir, O_TMPFILE | O_RDWR, 0600);
  if (!fd)
    PERFETTO_PLOG("Could not create a temporary trace file in %s", kStateDir);
  return fd;
}

void PerfettoCmd::LogUploadEventAndroid(PerfettoStatsdAtom atom) {
  if (!is_uploading_)
    return;
  PERFETTO_LAZY_LOAD(android_internal::StatsdLogEvent, log_event_fn);
  base::Uuid uuid(uuid_);
  log_event_fn(atom, uuid.lsb(), uuid.msb());
}

}  // namespace perfetto
