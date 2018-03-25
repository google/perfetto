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

#include "perfetto_cmd.h"

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"

#include "perfetto/config/trace_config.pb.h"
#include "perfetto/trace/trace.pb.h"

#include "google/protobuf/io/zero_copy_stream_impl_lite.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#include <android/os/DropBoxManager.h>
#include <utils/Looper.h>
#include <utils/StrongPointer.h>
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)

// TODO(primiano): add the ability to pass the file descriptor directly to the
// traced service instead of receiving a copy of the slices and writing them
// from this process.
namespace perfetto {
namespace {

constexpr char kDefaultDropBoxTag[] = "perfetto";

std::string GetDirName(const std::string& path) {
  size_t sep = path.find_last_of('/');
  if (sep == std::string::npos)
    return ".";
  return path.substr(0, sep);
}

}  // namespace

// Temporary directory for DropBox traces. Note that this is automatically
// created by the system by setting setprop persist.traced.enable=1.
const char* kTempDropBoxTraceDir = "/data/misc/perfetto-traces";

using protozero::proto_utils::WriteVarInt;
using protozero::proto_utils::MakeTagLengthDelimited;

int PerfettoCmd::PrintUsage(const char* argv0) {
  PERFETTO_ELOG(R"(
Usage: %s
  --background     -b     : Exits immediately and continues tracing in background
  --config         -c     : /path/to/trace/config/file or - for stdin
  --out            -o     : /path/to/out/trace/file
  --dropbox        -d TAG : Upload trace into DropBox using tag TAG (default: %s)
  --no-guardrails  -n     : Ignore guardrails triggered when using --dropbox (for testing).
  --help           -h
)",
                argv0, kDefaultDropBoxTag);
  return 1;
}

int PerfettoCmd::Main(int argc, char** argv) {
  static const struct option long_options[] = {
      // |option_index| relies on the order of options, don't reshuffle them.
      {"help", required_argument, 0, 'h'},
      {"config", required_argument, 0, 'c'},
      {"out", required_argument, 0, 'o'},
      {"background", no_argument, 0, 'b'},
      {"dropbox", optional_argument, 0, 'd'},
      {"no-guardrails", optional_argument, 0, 'n'},
      {nullptr, 0, nullptr, 0}};

  int option_index = 0;
  std::string trace_config_raw;
  bool background = false;
  bool ignore_guardrails = false;
  for (;;) {
    int option =
        getopt_long(argc, argv, "c:o:bd::n", long_options, &option_index);

    if (option == -1)
      break;  // EOF.

    if (option == 'c') {
      if (strcmp(optarg, "-") == 0) {
        std::istreambuf_iterator<char> begin(std::cin), end;
        trace_config_raw.assign(begin, end);
      } else if (strcmp(optarg, ":test") == 0) {
        // TODO(primiano): temporary for testing only.
        perfetto::protos::TraceConfig test_config;
        test_config.add_buffers()->set_size_kb(4096);
        test_config.set_duration_ms(2000);
        auto* ds_config = test_config.add_data_sources()->mutable_config();
        ds_config->set_name("com.google.perfetto.ftrace");
        ds_config->mutable_ftrace_config()->add_ftrace_events("sched_switch");
        ds_config->mutable_ftrace_config()->add_ftrace_events("cpu_idle");
        ds_config->mutable_ftrace_config()->add_ftrace_events("cpu_frequency");
        ds_config->set_target_buffer(0);
        test_config.SerializeToString(&trace_config_raw);
      } else {
        if (!base::ReadFile(optarg, &trace_config_raw)) {
          PERFETTO_ELOG("Could not open %s", optarg);
          return 1;
        }
      }
      continue;
    }

    if (option == 'o') {
      trace_out_path_ = optarg;
      continue;
    }

    if (option == 'd') {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
      dropbox_tag_ = optarg ? optarg : kDefaultDropBoxTag;
      continue;
#else
      PERFETTO_ELOG("DropBox is only supported with Android tree builds");
      return 1;
#endif
    }

    if (option == 'b') {
      background = true;
      continue;
    }

    if (option == 'n') {
      ignore_guardrails = true;
      continue;
    }

    return PrintUsage(argv[0]);
  }

  if (!trace_out_path_.empty() && !dropbox_tag_.empty()) {
    PERFETTO_ELOG(
        "Can't log to a file (--out) and DropBox (--dropbox) at the same "
        "time");
    return 1;
  }

  if (trace_out_path_.empty() && dropbox_tag_.empty()) {
    return PrintUsage(argv[0]);
  }

  if (trace_config_raw.empty()) {
    PERFETTO_ELOG("The TraceConfig is empty");
    return 1;
  }

  perfetto::protos::TraceConfig trace_config_proto;
  PERFETTO_DLOG("Parsing TraceConfig, %zu bytes", trace_config_raw.size());
  bool parsed = trace_config_proto.ParseFromString(trace_config_raw);
  if (!parsed) {
    PERFETTO_ELOG("Could not parse TraceConfig proto from stdin");
    return 1;
  }
  trace_config_.reset(new TraceConfig());
  trace_config_->FromProto(trace_config_proto);
  trace_config_raw.clear();

  if (!OpenOutputFile())
    return 1;

  if (background) {
    PERFETTO_CHECK(daemon(0 /*nochdir*/, 0 /*noclose*/) == 0);
    PERFETTO_DLOG("Continuing in background");
  }

  RateLimiter limiter;
  RateLimiter::Args args{};
  args.is_dropbox = !dropbox_tag_.empty();
  args.current_time = base::GetWallTimeS();
  args.ignore_guardrails = ignore_guardrails;
  if (!limiter.ShouldTrace(args))
    return 1;

  consumer_endpoint_ = ConsumerIPCClient::Connect(PERFETTO_CONSUMER_SOCK_NAME,
                                                  this, &task_runner_);
  task_runner_.Run();

  return limiter.OnTraceDone(args, did_process_full_trace_,
                             bytes_uploaded_to_dropbox_)
             ? 0
             : 1;
}

void PerfettoCmd::OnConnect() {
  PERFETTO_LOG(
      "Connected to the Perfetto traced service, starting tracing for %d ms",
      trace_config_->duration_ms());
  PERFETTO_DCHECK(trace_config_);
  trace_config_->set_enable_extra_guardrails(!dropbox_tag_.empty());
  consumer_endpoint_->EnableTracing(*trace_config_);
  task_runner_.PostDelayedTask(std::bind(&PerfettoCmd::OnStopTraceTimer, this),
                               trace_config_->duration_ms());

  // Failsafe mechanism to avoid waiting indefinitely if the service hangs.
  task_runner_.PostDelayedTask(std::bind(&PerfettoCmd::OnTimeout, this),
                               trace_config_->duration_ms() * 2);
}

void PerfettoCmd::OnDisconnect() {
  PERFETTO_LOG("Disconnected from the Perfetto traced service");
  task_runner_.Quit();
}

void PerfettoCmd::OnStopTraceTimer() {
  PERFETTO_LOG("Timer expired, disabling tracing and collecting results");
  consumer_endpoint_->DisableTracing();
  consumer_endpoint_->ReadBuffers();
}

void PerfettoCmd::OnTimeout() {
  PERFETTO_ELOG("Timed out while waiting for trace from the service, aborting");
  task_runner_.Quit();
}

void PerfettoCmd::OnTraceData(std::vector<TracePacket> packets, bool has_more) {
  for (TracePacket& packet : packets) {
    for (const Slice& slice : packet.slices()) {
      uint8_t preamble[16];
      uint8_t* pos = preamble;
      pos = WriteVarInt(
          MakeTagLengthDelimited(protos::Trace::kPacketFieldNumber), pos);
      pos = WriteVarInt(static_cast<uint32_t>(slice.size), pos);
      fwrite(reinterpret_cast<const char*>(preamble), pos - preamble, 1,
             trace_out_stream_.get());
      fwrite(reinterpret_cast<const char*>(slice.start), slice.size, 1,
             trace_out_stream_.get());
    }
  }
  if (has_more)
    return;

  // Reached end of trace.
  task_runner_.Quit();

  fflush(*trace_out_stream_);
  long bytes_written = ftell(*trace_out_stream_);

  if (dropbox_tag_.empty()) {
    trace_out_stream_.reset();
    PERFETTO_CHECK(
        rename(tmp_trace_out_path_.c_str(), trace_out_path_.c_str()) == 0);
    PERFETTO_ILOG("Wrote %ld bytes into %s", bytes_written,
                  trace_out_path_.c_str());
  } else {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
    android::sp<android::os::DropBoxManager> dropbox =
        new android::os::DropBoxManager();
    fseek(*trace_out_stream_, 0, SEEK_SET);
    // DropBox takes ownership of the file descriptor, so give it a duplicate.
    // Also we need to give it a read-only copy of the fd or will hit a SELinux
    // violation (about system_server ending up with a writable FD to our dir).
    char fdpath[64];
    sprintf(fdpath, "/proc/self/fd/%d", fileno(*trace_out_stream_));
    base::ScopedFile read_only_fd(open(fdpath, O_RDONLY));
    PERFETTO_CHECK(read_only_fd);
    trace_out_stream_.reset();
    android::binder::Status status =
        dropbox->addFile(android::String16(dropbox_tag_.c_str()),
                         read_only_fd.release(), 0 /* flags */);
    if (!status.isOk()) {
      PERFETTO_ELOG("DropBox upload failed: %s", status.toString8().c_str());
      return;
    }
    // TODO(hjd): Account for compression.
    bytes_uploaded_to_dropbox_ = bytes_written;
    PERFETTO_ILOG("Uploaded %ld bytes into DropBox with tag %s", bytes_written,
                  dropbox_tag_.c_str());
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  }
  did_process_full_trace_ = true;
}

bool PerfettoCmd::OpenOutputFile() {
  base::ScopedFile fd;
  if (!dropbox_tag_.empty()) {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
    // If we are tracing to DropBox, there's no need to make a
    // filesystem-visible temporary file.
    // TODO(skyostil): Fall back to base::TempFile for older devices.
    fd.reset(open(kTempDropBoxTraceDir, O_TMPFILE | O_RDWR, 0600));
    if (!fd) {
      PERFETTO_ELOG("Could not create a temporary trace file in %s",
                    kTempDropBoxTraceDir);
      return false;
    }
#else
    PERFETTO_CHECK(false);
#endif
  } else {
    // Otherwise create a temporary file in the directory where the final trace
    // is going to be.
    tmp_trace_out_path_ = GetDirName(trace_out_path_) + "/perfetto-traceXXXXXX";
    fd.reset(mkstemp(&tmp_trace_out_path_[0]));
  }
  trace_out_stream_.reset(fdopen(fd.release(), "wb"));
  PERFETTO_CHECK(trace_out_stream_);
  return true;
}

int __attribute__((visibility("default")))
PerfettoCmdMain(int argc, char** argv) {
  perfetto::PerfettoCmd consumer_cmd;
  return consumer_cmd.Main(argc, argv);
}

}  // namespace perfetto
