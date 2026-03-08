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

#include "src/traced/probes/android_aflags/android_aflags_data_source.h"

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/config/android/android_aflags_config.pbzero.h"
#include "protos/perfetto/trace/android/android_aflags.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {
constexpr uint32_t kMinPollPeriodMs = 1000;
constexpr uint32_t kAflagsExitTimeoutMs = 1000;
}  // namespace

// static
const ProbesDataSource::Descriptor AndroidAflagsDataSource::descriptor = {
    /* name */ "android.aflags",
    /* flags */ Descriptor::kFlagsNone,
    /* fill_descriptor_func */ nullptr,
};

AndroidAflagsDataSource::AndroidAflagsDataSource(
    const DataSourceConfig& ds_config,
    base::TaskRunner* task_runner,
    TracingSessionID session_id,
    std::unique_ptr<TraceWriter> writer)
    : ProbesDataSource(session_id, &descriptor),
      task_runner_(task_runner),
      writer_(std::move(writer)),
      weak_factory_(this) {
  protos::pbzero::AndroidAflagsConfig::Decoder cfg(
      ds_config.android_aflags_config_raw());
  poll_period_ms_ = cfg.poll_ms();
  if (poll_period_ms_ > 0 && poll_period_ms_ < kMinPollPeriodMs) {
    PERFETTO_ILOG("poll_ms %" PRIu32 " is less than minimum of %" PRIu32
                  "ms. Increasing to %" PRIu32 "ms.",
                  poll_period_ms_, kMinPollPeriodMs, kMinPollPeriodMs);
    poll_period_ms_ = kMinPollPeriodMs;
  }
}

AndroidAflagsDataSource::~AndroidAflagsDataSource() {
  if (aflags_output_pipe_) {
    task_runner_->RemoveFileDescriptorWatch(*aflags_output_pipe_->rd);
  }
}

void AndroidAflagsDataSource::Start() {
  Tick();
}

void AndroidAflagsDataSource::Tick() {
  auto weak_this = weak_factory_.GetWeakPtr();
  if (poll_period_ms_ > 0) {
    uint32_t delay_ms =
        poll_period_ms_ -
        static_cast<uint32_t>(base::GetWallTimeMs().count() % poll_period_ms_);
    task_runner_->PostDelayedTask(
        [weak_this] {
          if (weak_this) {
            weak_this->Tick();
          }
        },
        delay_ms);
  }

  if (aflags_process_) {
    PERFETTO_DLOG("Aflags process still running, skipping tick.");
    return;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  aflags_output_pipe_ = base::Pipe::Create(base::Pipe::kRdNonBlock);
  // Watch the read end of the pipe for output from the aflags process.
  task_runner_->AddFileDescriptorWatch(*aflags_output_pipe_->rd, [weak_this] {
    if (weak_this) {
      weak_this->OnAflagsOutput();
    }
  });

  aflags_output_.clear();

  // It returns a base64-encoded binary proto that needs to be decoded before
  // being written to the trace.
  aflags_process_.emplace(std::initializer_list<std::string>{
      "/system/bin/aflags", "list", "--format", "proto"});
  aflags_process_->args.stdout_mode = base::Subprocess::OutputMode::kFd;
  aflags_process_->args.out_fd = std::move(aflags_output_pipe_->wr);
  aflags_process_->args.stderr_mode = base::Subprocess::OutputMode::kDevNull;
  aflags_process_->Start();
#else
  PERFETTO_ELOG("Aflags only supported on Android.");
#endif
}

void AndroidAflagsDataSource::OnAflagsOutput() {
  char buf[4096];
  for (;;) {
    ssize_t rsize = base::Read(*aflags_output_pipe_->rd, buf, sizeof(buf));
    if (rsize == 0) {
      break;  // EOF
    }

    if (rsize > 0) {
      aflags_output_.append(buf, static_cast<size_t>(rsize));
      continue;  // More data may be available, try reading again immediately.
    }

    if (base::IsAgain(errno)) {
      return;  // Read is blocked, wait for the next notification.
    }

    PERFETTO_PLOG("Error reading from aflags output pipe.");
    task_runner_->RemoveFileDescriptorWatch(*aflags_output_pipe_->rd);
    aflags_process_.reset();
    aflags_output_pipe_.reset();
    aflags_output_.clear();
    return;
  }

  task_runner_->RemoveFileDescriptorWatch(*aflags_output_pipe_->rd);
  FinalizeAflagsCapture();
}

void AndroidAflagsDataSource::FinalizeAflagsCapture() {
  if (!aflags_process_) {
    return;
  }

  aflags_process_->Poll();
  auto status = aflags_process_->status();
  if (status == base::Subprocess::kRunning) {
    // Process hasn't finished running yet, reschedule and check later.
    auto weak_this = weak_factory_.GetWeakPtr();
    task_runner_->PostDelayedTask(
        [weak_this] {
          if (weak_this)
            weak_this->FinalizeAflagsCapture();
        },
        kAflagsExitTimeoutMs);
    return;
  }

  auto returncode = aflags_process_->returncode();
  aflags_process_.reset();
  aflags_output_pipe_.reset();

  if (status != base::Subprocess::kTerminated || returncode != 0) {
    PERFETTO_ELOG("aflags failed: status: %d, code: %d", status, returncode);
    aflags_output_.clear();
    return;
  }

  std::string output = base::TrimWhitespace(aflags_output_);
  aflags_output_.clear();

  // The output of `aflags list --format proto` is base64-encoded.
  std::optional<std::string> decoded =
      base::Base64Decode(output.data(), output.size());
  if (!decoded) {
    PERFETTO_ELOG("Failed to decode aflags output (length: %zu)",
                  output.size());
    return;
  }

  TraceWriter::TracePacketHandle packet = writer_->NewTracePacket();
  packet->set_timestamp(static_cast<uint64_t>(base::GetBootTimeNs().count()));
  auto* aflags_proto = packet->set_android_aflags();
  aflags_proto->AppendRawProtoBytes(decoded->data(), decoded->size());
}

void AndroidAflagsDataSource::Flush(FlushRequestID,
                                    std::function<void()> callback) {
  writer_->Flush(callback);
}

}  // namespace perfetto
