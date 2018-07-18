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

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
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

#include "src/tracing/ipc/default_socket.h"

#include "google/protobuf/io/zero_copy_stream_impl_lite.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#include <android/os/DropBoxManager.h>
#include <utils/Looper.h>
#include <utils/StrongPointer.h>
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)

namespace perfetto {
namespace {

constexpr char kDefaultDropBoxTag[] = "perfetto";

perfetto::PerfettoCmd* g_consumer_cmd;

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
  --reset-guardrails      : Resets the state of the guardails and exits (for testing).
  --help           -h

statsd-specific flags:
  --alert-id           : ID of the alert that triggered this trace.
  --config-id          : ID of the triggering config.
  --config-uid         : UID of app which registered the config.
)",
                argv0, kDefaultDropBoxTag);
  return 1;
}

int PerfettoCmd::Main(int argc, char** argv) {
  enum LongOption {
    OPT_ALERT_ID = 1000,
    OPT_CONFIG_ID,
    OPT_CONFIG_UID,
    OPT_RESET_GUARDRAILS,
  };
  static const struct option long_options[] = {
      // |option_index| relies on the order of options, don't reshuffle them.
      {"help", required_argument, nullptr, 'h'},
      {"config", required_argument, nullptr, 'c'},
      {"out", required_argument, nullptr, 'o'},
      {"background", no_argument, nullptr, 'b'},
      {"dropbox", optional_argument, nullptr, 'd'},
      {"no-guardrails", optional_argument, nullptr, 'n'},
      {"alert-id", required_argument, nullptr, OPT_ALERT_ID},
      {"config-id", required_argument, nullptr, OPT_CONFIG_ID},
      {"config-uid", required_argument, nullptr, OPT_CONFIG_UID},
      {"reset-guardrails", no_argument, nullptr, OPT_RESET_GUARDRAILS},
      {nullptr, 0, nullptr, 0}};

  int option_index = 0;
  std::string trace_config_raw;
  bool background = false;
  bool ignore_guardrails = false;
  perfetto::protos::TraceConfig::StatsdMetadata statsd_metadata;
  RateLimiter limiter;

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
        ds_config->set_name("linux.ftrace");
        ds_config->mutable_ftrace_config()->add_ftrace_events("sched_switch");
        ds_config->mutable_ftrace_config()->add_ftrace_events("cpu_idle");
        ds_config->mutable_ftrace_config()->add_ftrace_events("cpu_frequency");
        ds_config->set_target_buffer(0);
        test_config.SerializeToString(&trace_config_raw);
      } else {
        if (!base::ReadFile(optarg, &trace_config_raw)) {
          PERFETTO_PLOG("Could not open %s", optarg);
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

    if (option == OPT_RESET_GUARDRAILS) {
      PERFETTO_CHECK(limiter.ClearState());
      PERFETTO_ILOG("Guardrail state cleared");
      return 0;
    }

    if (option == OPT_ALERT_ID) {
      statsd_metadata.set_triggering_alert_id(atoll(optarg));
      continue;
    }

    if (option == OPT_CONFIG_ID) {
      statsd_metadata.set_triggering_config_id(atoll(optarg));
      continue;
    }

    if (option == OPT_CONFIG_UID) {
      statsd_metadata.set_triggering_config_uid(atoi(optarg));
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
  *trace_config_proto.mutable_statsd_metadata() = std::move(statsd_metadata);
  trace_config_.reset(new TraceConfig());
  trace_config_->FromProto(trace_config_proto);
  trace_config_raw.clear();

  if (!OpenOutputFile())
    return 1;

  if (background) {
    PERFETTO_CHECK(daemon(0 /*nochdir*/, 0 /*noclose*/) == 0);
    PERFETTO_DLOG("Continuing in background");
  }

  RateLimiter::Args args{};
  args.is_dropbox = !dropbox_tag_.empty();
  args.current_time = base::GetWallTimeS();
  args.ignore_guardrails = ignore_guardrails;
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_USERDEBUG_BUILD)
  args.max_upload_bytes_override =
      trace_config_->guardrail_overrides().max_upload_per_day_bytes();
#endif
  if (!limiter.ShouldTrace(args))
    return 1;

  consumer_endpoint_ =
      ConsumerIPCClient::Connect(GetConsumerSocket(), this, &task_runner_);
  SetupCtrlCSignalHandler();
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

  base::ScopedFile optional_fd;
  if (trace_config_->write_into_file())
    optional_fd.reset(dup(fileno(*trace_out_stream_)));

  consumer_endpoint_->EnableTracing(*trace_config_, std::move(optional_fd));

  // Failsafe mechanism to avoid waiting indefinitely if the service hangs.
  if (trace_config_->duration_ms()) {
    task_runner_.PostDelayedTask(std::bind(&PerfettoCmd::OnTimeout, this),
                                 trace_config_->duration_ms() + 10000);
  }
}

void PerfettoCmd::OnDisconnect() {
  PERFETTO_LOG("Disconnected from the Perfetto traced service");
  task_runner_.Quit();
}

void PerfettoCmd::OnTimeout() {
  PERFETTO_ELOG("Timed out while waiting for trace from the service, aborting");
  task_runner_.Quit();
}

void PerfettoCmd::OnTraceData(std::vector<TracePacket> packets, bool has_more) {
  for (TracePacket& packet : packets) {
    uint8_t preamble[16];
    uint8_t* pos = preamble;
    // ID of the |packet| field in trace.proto. Hardcoded as this we not depend
    // on protos/trace:lite for binary size saving reasons.
    static constexpr uint32_t kPacketFieldNumber = 1;
    pos = WriteVarInt(MakeTagLengthDelimited(kPacketFieldNumber), pos);
    pos = WriteVarInt(static_cast<uint32_t>(packet.size()), pos);
    fwrite(reinterpret_cast<const char*>(preamble),
           static_cast<size_t>(pos - preamble), 1, trace_out_stream_.get());
    for (const Slice& slice : packet.slices()) {
      fwrite(reinterpret_cast<const char*>(slice.start), slice.size, 1,
             trace_out_stream_.get());
    }
  }

  if (!has_more)
    FinalizeTraceAndExit();  // Reached end of trace.
}

void PerfettoCmd::OnTracingDisabled() {
  if (trace_config_->write_into_file()) {
    // If write_into_file == true, at this point the passed file contains
    // already all the packets.
    return FinalizeTraceAndExit();
  }
  // This will cause a bunch of OnTraceData callbacks. The last one will
  // save the file and exit.
  consumer_endpoint_->ReadBuffers();
}

void PerfettoCmd::FinalizeTraceAndExit() {
  fflush(*trace_out_stream_);
  fseek(*trace_out_stream_, 0, SEEK_END);
  long bytes_written = ftell(*trace_out_stream_);
  if (dropbox_tag_.empty()) {
    trace_out_stream_.reset();
    did_process_full_trace_ = true;
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
    if (status.isOk()) {
      // TODO(hjd): Account for compression.
      did_process_full_trace_ = true;
      bytes_uploaded_to_dropbox_ = bytes_written;
      PERFETTO_ILOG("Uploaded %ld bytes into DropBox with tag %s",
                    bytes_written, dropbox_tag_.c_str());
    } else {
      PERFETTO_ELOG("DropBox upload failed: %s", status.toString8().c_str());
    }
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  }
  task_runner_.Quit();
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
    PERFETTO_FATAL("Tracing to Dropbox requires the Android build.");
#endif
  } else {
    fd.reset(open(trace_out_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600));
  }
  trace_out_stream_.reset(fdopen(fd.release(), "wb"));
  PERFETTO_CHECK(trace_out_stream_);
  return true;
}

void PerfettoCmd::SetupCtrlCSignalHandler() {
  // Setup the pipe used to deliver the CTRL-C notification from signal handler.
  int pipe_fds[2];
  PERFETTO_CHECK(pipe(pipe_fds) == 0);
  ctrl_c_pipe_rd_.reset(pipe_fds[0]);
  ctrl_c_pipe_wr_.reset(pipe_fds[1]);

  // Setup signal handler.
  struct sigaction sa {};

// Glibc headers for sa_sigaction trigger this.
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  sa.sa_handler = [](int) {
    PERFETTO_LOG("SIGINT received: disabling tracing");
    char one = '1';
    PERFETTO_CHECK(PERFETTO_EINTR(write(g_consumer_cmd->ctrl_c_pipe_wr(), &one,
                                        sizeof(one))) == 1);
  };
  sa.sa_flags = static_cast<decltype(sa.sa_flags)>(SA_RESETHAND | SA_RESTART);
#pragma GCC diagnostic pop
  sigaction(SIGINT, &sa, nullptr);

  task_runner_.AddFileDescriptorWatch(
      *ctrl_c_pipe_rd_, [this] { consumer_endpoint_->DisableTracing(); });
}

int __attribute__((visibility("default")))
PerfettoCmdMain(int argc, char** argv) {
  g_consumer_cmd = new perfetto::PerfettoCmd();
  return g_consumer_cmd->Main(argc, argv);
}

}  // namespace perfetto
