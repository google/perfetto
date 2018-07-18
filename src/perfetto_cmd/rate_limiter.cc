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

#include "src/perfetto_cmd/rate_limiter.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/utils.h"
#include "src/perfetto_cmd/perfetto_cmd.h"

namespace perfetto {
namespace {

// 5 mins between traces.
const uint64_t kCooldownInSeconds = 60 * 5;

// Every 24 hours we reset how much we've uploaded.
const uint64_t kMaxUploadResetPeriodInSeconds = 60 * 60 * 24;

// Maximum of 10mb every 24h.
const uint64_t kMaxUploadInBytes = 1024 * 1024 * 10;

}  // namespace

RateLimiter::RateLimiter() = default;
RateLimiter::~RateLimiter() = default;

bool RateLimiter::ShouldTrace(const Args& args) {
  uint64_t now_in_s = static_cast<uint64_t>(args.current_time.count());

  // Not uploading?
  // -> We can just trace.
  if (!args.is_dropbox)
    return true;

  // The state file is gone.
  // Maybe we're tracing for the first time or maybe something went wrong the
  // last time we tried to save the state. Either way reinitialize the state
  // file.
  if (!StateFileExists()) {
    // We can't write the empty state file?
    // -> Give up.
    if (!ClearState()) {
      PERFETTO_ELOG("Guardrail: failed to initialize guardrail state.");
      return false;
    }
  }

  bool loaded_state = LoadState(&state_);

  // Failed to load the state?
  // Current time is before either saved times?
  // Last saved trace time is before first saved trace time?
  // -> Try to save a clean state but don't trace.
  if (!loaded_state || now_in_s < state_.first_trace_timestamp() ||
      now_in_s < state_.last_trace_timestamp() ||
      state_.last_trace_timestamp() < state_.first_trace_timestamp()) {
    ClearState();
    PERFETTO_ELOG("Guardrail: state invalid, clearing it.");
    if (!args.ignore_guardrails)
      return false;
  }

  // If we've uploaded in the last 5mins we shouldn't trace now.
  if ((now_in_s - state_.last_trace_timestamp()) < kCooldownInSeconds) {
    PERFETTO_ELOG("Guardrail: Uploaded to DropBox in the last 5mins.");
    if (!args.ignore_guardrails)
      return false;
  }

  // First trace was more than 24h ago? Reset state.
  if ((now_in_s - state_.first_trace_timestamp()) >
      kMaxUploadResetPeriodInSeconds) {
    state_.set_first_trace_timestamp(0);
    state_.set_last_trace_timestamp(0);
    state_.set_total_bytes_uploaded(0);
    return true;
  }

  // If we've uploaded more than 10mb in the last 24 hours we shouldn't trace
  // now.
  uint64_t max_upload_guardrail = args.max_upload_bytes_override > 0
                                      ? args.max_upload_bytes_override
                                      : kMaxUploadInBytes;
  if (state_.total_bytes_uploaded() > max_upload_guardrail) {
    PERFETTO_ELOG("Guardrail: Uploaded >10mb DropBox in the last 24h.");
    if (!args.ignore_guardrails)
      return false;
  }

  return true;
}

bool RateLimiter::OnTraceDone(const Args& args, bool success, size_t bytes) {
  uint64_t now_in_s = static_cast<uint64_t>(args.current_time.count());

  // Failed to upload? Don't update the state.
  if (!success)
    return false;

  if (!args.is_dropbox)
    return true;

  // If the first trace timestamp is 0 (either because this is the
  // first time or because it was reset for being more than 24h ago).
  // -> We update it to the time of this trace.
  if (state_.first_trace_timestamp() == 0)
    state_.set_first_trace_timestamp(now_in_s);
  // Always updated the last trace timestamp.
  state_.set_last_trace_timestamp(now_in_s);
  // Add the amount we uploaded to the running total.
  state_.set_total_bytes_uploaded(state_.total_bytes_uploaded() + bytes);

  if (!SaveState(state_)) {
    PERFETTO_ELOG("Failed to save state.");
    return false;
  }

  return true;
}

std::string RateLimiter::GetStateFilePath() const {
  return std::string(kTempDropBoxTraceDir) + "/.guardraildata";
}

bool RateLimiter::StateFileExists() {
  struct stat out;
  return stat(GetStateFilePath().c_str(), &out) != -1;
}

bool RateLimiter::ClearState() {
  PerfettoCmdState zero{};
  zero.set_total_bytes_uploaded(0);
  zero.set_last_trace_timestamp(0);
  zero.set_first_trace_timestamp(0);
  bool success = SaveState(zero);
  if (!success && StateFileExists())
    remove(GetStateFilePath().c_str());
  return success;
}

bool RateLimiter::LoadState(PerfettoCmdState* state) {
  base::ScopedFile in_fd;
  in_fd.reset(open(GetStateFilePath().c_str(), O_RDONLY));

  if (!in_fd)
    return false;
  char buf[1024];
  ssize_t bytes = PERFETTO_EINTR(read(in_fd.get(), &buf, sizeof(buf)));
  if (bytes <= 0)
    return false;
  return state->ParseFromArray(&buf, static_cast<int>(bytes));
}

bool RateLimiter::SaveState(const PerfettoCmdState& state) {
  base::ScopedFile out_fd;
  // Rationale for 0666: the cmdline client can be executed under two
  // different Unix UIDs: shell and statsd. If we run one after the
  // other and the file has 0600 permissions, then the 2nd run won't
  // be able to read the file and will clear it, aborting the trace.
  // SELinux still prevents that anything other than the perfetto
  // executable can change the guardrail file.
  out_fd.reset(
      open(GetStateFilePath().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666));
  if (!out_fd)
    return false;
  char buf[1024];
  size_t size = static_cast<size_t>(state.ByteSize());
  PERFETTO_CHECK(size < sizeof(buf));
  if (!state.SerializeToArray(&buf, static_cast<int>(size)))
    return false;
  ssize_t written = PERFETTO_EINTR(write(out_fd.get(), &buf, size));
  return written >= 0 && static_cast<size_t>(written) == size;
}

}  // namespace perfetto
