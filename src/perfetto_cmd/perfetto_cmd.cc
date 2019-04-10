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
#include "perfetto/base/string_view.h"
#include "perfetto/base/time.h"
#include "perfetto/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/basic_types.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "src/perfetto_cmd/config.h"
#include "src/perfetto_cmd/pbtxt_to_pb.h"
#include "src/perfetto_cmd/trigger_producer.h"

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

perfetto::PerfettoCmd* g_consumer_cmd;

class LoggingErrorReporter : public ErrorReporter {
 public:
  LoggingErrorReporter(std::string file_name, const char* config)
      : file_name_(file_name), config_(config) {}

  void AddError(size_t row,
                size_t column,
                size_t length,
                const std::string& message) override {
    parsed_successfully_ = false;
    std::string line = ExtractLine(row - 1).ToStdString();
    if (!line.empty() && line[line.length() - 1] == '\n') {
      line.erase(line.length() - 1);
    }

    std::string guide(column + length, ' ');
    for (size_t i = column; i < column + length; i++) {
      guide[i - 1] = i == column ? '^' : '~';
    }
    fprintf(stderr, "%s:%zu:%zu error: %s\n", file_name_.c_str(), row, column,
            message.c_str());
    fprintf(stderr, "%s\n", line.c_str());
    fprintf(stderr, "%s\n", guide.c_str());
  }

  bool Success() const { return parsed_successfully_; }

 private:
  base::StringView ExtractLine(size_t line) {
    const char* start = config_;
    const char* end = config_;

    for (size_t i = 0; i < line + 1; i++) {
      start = end;
      char c;
      while ((c = *end++) && c != '\n')
        ;
    }
    return base::StringView(start, static_cast<size_t>(end - start));
  }

  bool parsed_successfully_ = true;
  std::string file_name_;
  const char* config_;
};

bool ParseTraceConfigPbtxt(const std::string& file_name,
                           const std::string& pbtxt,
                           protos::TraceConfig* config) {
  LoggingErrorReporter reporter(file_name, pbtxt.c_str());
  std::vector<uint8_t> buf = PbtxtToPb(pbtxt, &reporter);
  if (!reporter.Success())
    return false;
  if (!config->ParseFromArray(buf.data(), static_cast<int>(buf.size())))
    return false;
  return true;
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
  --background     -d      : Exits immediately and continues tracing in background
  --config         -c      : /path/to/trace/config/file or - for stdin
  --out            -o      : /path/to/out/trace/file or - for stdout
  --dropbox           TAG  : Upload trace into DropBox using tag TAG
  --no-guardrails          : Ignore guardrails triggered when using --dropbox (for testing).
  --txt                    : Parse config as pbtxt. Not a stable API. Not for production use.
  --reset-guardrails       : Resets the state of the guardails and exits (for testing).
  --trigger           NAME : Activate the NAME on to the service. If specified multiple times
                             will activate them all. Cannot be used with --config or
                             configuration flags.
  --help           -h


light configuration flags: (only when NOT using -c/--config)
  --time           -t      : Trace duration N[s,m,h] (default: 10s)
  --buffer         -b      : Ring buffer size N[mb,gb] (default: 32mb)
  --size           -s      : Max file size N[mb,gb] (default: in-memory ring-buffer only)
  ATRACE_CAT               : Record ATRACE_CAT (e.g. wm)
  FTRACE_GROUP/FTRACE_NAME : Record ftrace event (e.g. sched/sched_switch)
  FTRACE_GROUP/*           : Record all events in group (e.g. sched/*)


statsd-specific flags:
  --alert-id           : ID of the alert that triggered this trace.
  --config-id          : ID of the triggering config.
  --config-uid         : UID of app which registered the config.
  --subscription-id    : ID of the subscription that triggered this trace.

Detach mode. DISCOURAGED, read https://docs.perfetto.dev/#/detached-mode :
  --detach=key          : Detach from the tracing session with the given key.
  --attach=key [--stop] : Re-attach to the session (optionally stop tracing once reattached).
  --is_detached=key     : Check if the session can be re-attached (0:Yes, 2:No, 1:Error).
)",
                argv0);
  return 1;
}

int PerfettoCmd::Main(int argc, char** argv) {
  enum LongOption {
    OPT_ALERT_ID = 1000,
    OPT_CONFIG_ID,
    OPT_CONFIG_UID,
    OPT_SUBSCRIPTION_ID,
    OPT_RESET_GUARDRAILS,
    OPT_TRIGGER,
    OPT_PBTXT_CONFIG,
    OPT_DROPBOX,
    OPT_ATRACE_APP,
    OPT_IGNORE_GUARDRAILS,
    OPT_DETACH,
    OPT_ATTACH,
    OPT_IS_DETACHED,
    OPT_STOP,
  };
  static const struct option long_options[] = {
      {"help", no_argument, nullptr, 'h'},
      {"config", required_argument, nullptr, 'c'},
      {"out", required_argument, nullptr, 'o'},
      {"background", no_argument, nullptr, 'd'},
      {"time", required_argument, nullptr, 't'},
      {"buffer", required_argument, nullptr, 'b'},
      {"size", required_argument, nullptr, 's'},
      {"no-guardrails", no_argument, nullptr, OPT_IGNORE_GUARDRAILS},
      {"txt", no_argument, nullptr, OPT_PBTXT_CONFIG},
      {"dropbox", required_argument, nullptr, OPT_DROPBOX},
      {"alert-id", required_argument, nullptr, OPT_ALERT_ID},
      {"config-id", required_argument, nullptr, OPT_CONFIG_ID},
      {"config-uid", required_argument, nullptr, OPT_CONFIG_UID},
      {"subscription-id", required_argument, nullptr, OPT_SUBSCRIPTION_ID},
      {"reset-guardrails", no_argument, nullptr, OPT_RESET_GUARDRAILS},
      {"trigger", required_argument, nullptr, OPT_TRIGGER},
      {"detach", required_argument, nullptr, OPT_DETACH},
      {"attach", required_argument, nullptr, OPT_ATTACH},
      {"is_detached", required_argument, nullptr, OPT_IS_DETACHED},
      {"stop", no_argument, nullptr, OPT_STOP},
      {"app", required_argument, nullptr, OPT_ATRACE_APP},
      {nullptr, 0, nullptr, 0}};

  int option_index = 0;
  std::string config_file_name;
  std::string trace_config_raw;
  bool background = false;
  bool ignore_guardrails = false;
  bool parse_as_pbtxt = false;
  perfetto::protos::TraceConfig::StatsdMetadata statsd_metadata;
  RateLimiter limiter;

  ConfigOptions config_options;
  bool has_config_options = false;
  std::vector<std::string> triggers_to_activate;

  for (;;) {
    int option =
        getopt_long(argc, argv, "hc:o:dt:b:s:", long_options, &option_index);

    if (option == -1)
      break;  // EOF.

    if (option == 'c') {
      config_file_name = std::string(optarg);
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
      background = true;
      continue;
    }
    if (option == 't') {
      has_config_options = true;
      config_options.time = std::string(optarg);
      continue;
    }

    if (option == 'b') {
      has_config_options = true;
      config_options.buffer_size = std::string(optarg);
      continue;
    }

    if (option == 's') {
      has_config_options = true;
      config_options.max_file_size = std::string(optarg);
      continue;
    }

    if (option == OPT_DROPBOX) {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
      if (!optarg)
        PERFETTO_FATAL("optarg is null");
      dropbox_tag_ = optarg;
      continue;
#else
      PERFETTO_ELOG("DropBox is only supported with Android tree builds");
      return 1;
#endif
    }

    if (option == OPT_PBTXT_CONFIG) {
      parse_as_pbtxt = true;
      continue;
    }

    if (option == OPT_IGNORE_GUARDRAILS) {
      ignore_guardrails = true;
      continue;
    }

    if (option == OPT_RESET_GUARDRAILS) {
      PERFETTO_CHECK(limiter.ClearState());
      PERFETTO_ILOG("Guardrail state cleared");
      return 0;
    }

    if (option == OPT_TRIGGER) {
      triggers_to_activate.push_back(std::string(optarg));
      continue;
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

    if (option == OPT_SUBSCRIPTION_ID) {
      statsd_metadata.set_triggering_subscription_id(atoll(optarg));
      continue;
    }

    if (option == OPT_ATRACE_APP) {
      config_options.atrace_apps.push_back(std::string(optarg));
      has_config_options = true;
      continue;
    }

    if (option == OPT_DETACH) {
      detach_key_ = std::string(optarg);
      PERFETTO_CHECK(!detach_key_.empty());
      continue;
    }

    if (option == OPT_ATTACH) {
      attach_key_ = std::string(optarg);
      PERFETTO_CHECK(!attach_key_.empty());
      continue;
    }

    if (option == OPT_IS_DETACHED) {
      attach_key_ = std::string(optarg);
      redetach_once_attached_ = true;
      PERFETTO_CHECK(!attach_key_.empty());
      continue;
    }

    if (option == OPT_STOP) {
      stop_trace_once_attached_ = true;
      continue;
    }

    return PrintUsage(argv[0]);
  }

  for (ssize_t i = optind; i < argc; i++) {
    has_config_options = true;
    config_options.categories.push_back(argv[i]);
  }

  if (is_detach() && is_attach()) {
    PERFETTO_ELOG("--attach and --detach are mutually exclusive");
    return 1;
  }

  if (is_detach() && background) {
    PERFETTO_ELOG("--detach and --background are mutually exclusive");
    return 1;
  }

  if (stop_trace_once_attached_ && !is_attach()) {
    PERFETTO_ELOG("--stop is supported only in combination with --attach");
    return 1;
  }

  // Parse the trace config. It can be either:
  // 1) A proto-encoded file/stdin (-c ...).
  // 2) A proto-text file/stdin (-c ... --txt).
  // 3) A set of option arguments (-t 10s -s 10m).
  // The only cases in which a trace config is not expected is --attach or
  // --trigger. For both of these we are just acting on already
  // existing sessions.
  perfetto::protos::TraceConfig trace_config_proto;
  bool parsed = false;
  if (is_attach()) {
    if ((!trace_config_raw.empty() || has_config_options)) {
      PERFETTO_ELOG("Cannot specify a trace config with --attach");
      return 1;
    }
    if (!triggers_to_activate.empty()) {
      PERFETTO_ELOG("Cannot specify triggers to activate with --attach");
      return 1;
    }
  } else if (!triggers_to_activate.empty()) {
    if (!trace_config_raw.empty() || has_config_options) {
      PERFETTO_ELOG("Cannot specify a trace config with --trigger");
      return 1;
    }
  } else if (has_config_options) {
    if (!trace_config_raw.empty()) {
      PERFETTO_ELOG(
          "Cannot specify both -c/--config and any of --time, --size, "
          "--buffer, --app, ATRACE_CAT, FTRACE_EVENT");
      return 1;
    }
    parsed = CreateConfigFromOptions(config_options, &trace_config_proto);
  } else {
    if (trace_config_raw.empty()) {
      PERFETTO_ELOG("The TraceConfig is empty");
      return 1;
    }
    PERFETTO_DLOG("Parsing TraceConfig, %zu bytes", trace_config_raw.size());
    if (parse_as_pbtxt) {
      parsed = ParseTraceConfigPbtxt(config_file_name, trace_config_raw,
                                     &trace_config_proto);
    } else {
      parsed = trace_config_proto.ParseFromString(trace_config_raw);
    }
  }

  trace_config_.reset(new TraceConfig());
  if (parsed) {
    *trace_config_proto.mutable_statsd_metadata() = std::move(statsd_metadata);
    trace_config_->FromProto(trace_config_proto);
    trace_config_raw.clear();
  } else if (!is_attach() && triggers_to_activate.empty()) {
    PERFETTO_ELOG("The trace config is invalid, bailing out.");
    return 1;
  }

  // Set up the output file. Either --out or --dropbox are expected, with the
  // only exception of --attach. In this case the output file is passed when
  // detaching.
  if (!trace_out_path_.empty() && !dropbox_tag_.empty()) {
    PERFETTO_ELOG(
        "Can't log to a file (--out) and DropBox (--dropbox) at the same "
        "time");
    return 1;
  }

  bool open_out_file = true;
  if (is_attach()) {
    open_out_file = false;
    if (!trace_out_path_.empty() || !dropbox_tag_.empty()) {
      PERFETTO_ELOG("Can't pass an --out file (or --dropbox) to --attach");
      return 1;
    }
  } else if (!triggers_to_activate.empty()) {
    open_out_file = false;
  } else if (trace_out_path_.empty() && dropbox_tag_.empty()) {
    PERFETTO_ELOG("Either --out or --dropbox is required");
    return 1;
  } else if (is_detach() && !trace_config_->write_into_file()) {
    // In detached mode we must pass the file descriptor to the service and
    // let that one write the trace. We cannot use the IPC readback code path
    // because the client process is about to exit soon after detaching.
    PERFETTO_ELOG(
        "TraceConfig's write_into_file must be true when using --detach");
    return 1;
  }
  if (open_out_file && !OpenOutputFile())
    return 1;

  if (background) {
    pid_t pid;
    switch (pid = fork()) {
      case -1:
        PERFETTO_FATAL("fork");
      case 0: {
        PERFETTO_CHECK(setsid() != -1);
        base::ignore_result(chdir("/"));
        base::ScopedFile null = base::OpenFile("/dev/null", O_RDONLY);
        PERFETTO_CHECK(null);
        PERFETTO_CHECK(dup2(*null, STDIN_FILENO) != -1);
        PERFETTO_CHECK(dup2(*null, STDOUT_FILENO) != -1);
        PERFETTO_CHECK(dup2(*null, STDERR_FILENO) != -1);
        // Do not accidentally close stdin/stdout/stderr.
        if (*null <= 2)
          null.release();
        break;
      }
      default:
        printf("%d\n", pid);
        exit(0);
    }
  }

  // If we are just activating triggers then we don't need to rate limit,
  // connect as a consumer or run the trace. So bail out after processing all
  // the options.
  if (!triggers_to_activate.empty()) {
    bool finished_with_success = false;
    TriggerProducer producer(&task_runner_,
                             [this, &finished_with_success](bool success) {
                               finished_with_success = success;
                               task_runner_.Quit();
                             },
                             &triggers_to_activate);
    task_runner_.Run();
    return finished_with_success ? 0 : 1;
  }

  RateLimiter::Args args{};
  args.is_dropbox = !dropbox_tag_.empty();
  args.current_time = base::GetWallTimeS();
  args.ignore_guardrails = ignore_guardrails;
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_USERDEBUG_BUILD) || \
    PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
  args.max_upload_bytes_override =
      trace_config_->guardrail_overrides().max_upload_per_day_bytes();
#endif
  if (!limiter.ShouldTrace(args))
    return 1;

  consumer_endpoint_ =
      ConsumerIPCClient::Connect(GetConsumerSocket(), this, &task_runner_);
  SetupCtrlCSignalHandler();
  task_runner_.Run();

  return limiter.OnTraceDone(args, did_process_full_trace_, bytes_written_) ? 0
                                                                            : 1;
}

void PerfettoCmd::OnConnect() {
  if (is_attach()) {
    consumer_endpoint_->Attach(attach_key_);
    return;
  }

  PERFETTO_LOG(
      "Connected to the Perfetto traced service, starting tracing for %d ms",
      trace_config_->duration_ms());
  PERFETTO_DCHECK(trace_config_);
  trace_config_->set_enable_extra_guardrails(!dropbox_tag_.empty());

  base::ScopedFile optional_fd;
  if (trace_config_->write_into_file())
    optional_fd.reset(dup(fileno(*trace_out_stream_)));

  consumer_endpoint_->EnableTracing(*trace_config_, std::move(optional_fd));

  if (is_detach()) {
    consumer_endpoint_->Detach(detach_key_);  // Will invoke OnDetach() soon.
    return;
  }

  // Failsafe mechanism to avoid waiting indefinitely if the service hangs.
  if (trace_config_->duration_ms()) {
    uint32_t trace_timeout = trace_config_->duration_ms() + 10000 +
                             trace_config_->flush_timeout_ms();
    task_runner_.PostDelayedTask(std::bind(&PerfettoCmd::OnTimeout, this),
                                 trace_timeout);
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
    bytes_written_ +=
        fwrite(reinterpret_cast<const char*>(preamble), 1,
               static_cast<size_t>(pos - preamble), trace_out_stream_.get());
    for (const Slice& slice : packet.slices()) {
      bytes_written_ += fwrite(reinterpret_cast<const char*>(slice.start), 1,
                               slice.size, trace_out_stream_.get());
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
  if (dropbox_tag_.empty()) {
    trace_out_stream_.reset();
    did_process_full_trace_ = true;
    if (trace_config_->write_into_file()) {
      // trace_out_path_ might be empty in the case of --attach.
      PERFETTO_ILOG("Trace written into the output file");
    } else {
      PERFETTO_ILOG(
          "Wrote %" PRIu64 " bytes into %s", bytes_written_,
          trace_out_path_ == "-" ? "stdout" : trace_out_path_.c_str());
    }
  } else {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
    if (bytes_written_ == 0) {
      PERFETTO_ILOG("Skipping upload to dropbox. Empty trace.");
      did_process_full_trace_ = true;
      task_runner_.Quit();
      return;
    }
    android::sp<android::os::DropBoxManager> dropbox =
        new android::os::DropBoxManager();
    fseek(*trace_out_stream_, 0, SEEK_SET);
    // DropBox takes ownership of the file descriptor, so give it a duplicate.
    // Also we need to give it a read-only copy of the fd or will hit a SELinux
    // violation (about system_server ending up with a writable FD to our dir).
    char fdpath[64];
    sprintf(fdpath, "/proc/self/fd/%d", fileno(*trace_out_stream_));
    base::ScopedFile read_only_fd(base::OpenFile(fdpath, O_RDONLY));
    PERFETTO_CHECK(read_only_fd);
    trace_out_stream_.reset();
    android::binder::Status status =
        dropbox->addFile(android::String16(dropbox_tag_.c_str()),
                         read_only_fd.release(), 0 /* flags */);
    if (status.isOk()) {
      // TODO(hjd): Account for compression.
      did_process_full_trace_ = true;
      PERFETTO_ILOG("Uploaded %" PRIu64 " bytes into DropBox with tag %s",
                    bytes_written_, dropbox_tag_.c_str());
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
    fd = base::OpenFile(kTempDropBoxTraceDir, O_TMPFILE | O_RDWR, 0600);
    if (!fd) {
      PERFETTO_ELOG("Could not create a temporary trace file in %s",
                    kTempDropBoxTraceDir);
      return false;
    }
#else
    PERFETTO_FATAL("Tracing to Dropbox requires the Android build.");
#endif
  } else if (trace_out_path_ == "-") {
    fd.reset(dup(STDOUT_FILENO));
  } else {
    fd = base::OpenFile(trace_out_path_, O_RDWR | O_CREAT | O_TRUNC, 0600);
  }
  trace_out_stream_.reset(fdopen(fd.release(), "wb"));
  PERFETTO_CHECK(trace_out_stream_);
  return true;
}

void PerfettoCmd::SetupCtrlCSignalHandler() {
  // Setup signal handler.
  struct sigaction sa {};

// Glibc headers for sa_sigaction trigger this.
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  sa.sa_handler = [](int) { g_consumer_cmd->SignalCtrlC(); };
  sa.sa_flags = static_cast<decltype(sa.sa_flags)>(SA_RESETHAND | SA_RESTART);
#pragma GCC diagnostic pop
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  task_runner_.AddFileDescriptorWatch(ctrl_c_evt_.fd(), [this] {
    PERFETTO_LOG("SIGINT/SIGTERM received: disabling tracing.");
    ctrl_c_evt_.Clear();
    consumer_endpoint_->Flush(0, [this](bool flush_success) {
      if (!flush_success)
        PERFETTO_ELOG("Final flush unsuccessful.");
      consumer_endpoint_->DisableTracing();
    });
  });
}

void PerfettoCmd::OnDetach(bool success) {
  if (!success) {
    PERFETTO_ELOG("Session detach failed");
    exit(1);
  }
  exit(0);
}

void PerfettoCmd::OnAttach(bool success, const TraceConfig& trace_config) {
  if (!success) {
    if (!redetach_once_attached_) {
      // Print an error message if attach fails, with the exception of the
      // --is_detached case, where we want to silently return.
      PERFETTO_ELOG("Session re-attach failed. Check service logs for details");
    }
    // Keep this exit code distinguishable from the general error code so
    // --is_detached can tell the difference between a general error and the
    // not-detached case.
    exit(2);
  }

  if (redetach_once_attached_) {
    consumer_endpoint_->Detach(attach_key_);  // Will invoke OnDetach() soon.
    return;
  }

  trace_config_.reset(new TraceConfig(trace_config));
  PERFETTO_DCHECK(trace_config_->write_into_file());

  if (stop_trace_once_attached_) {
    consumer_endpoint_->Flush(0, [this](bool flush_success) {
      if (!flush_success)
        PERFETTO_ELOG("Final flush unsuccessful.");
      consumer_endpoint_->DisableTracing();
    });
  }
}

void PerfettoCmd::OnTraceStats(bool /*success*/,
                               const TraceStats& /*trace_config*/) {
  // TODO(eseckler): Support GetTraceStats().
}

void PerfettoCmd::OnObservableEvents(
    const ObservableEvents& /*observable_events*/) {}

int __attribute__((visibility("default")))
PerfettoCmdMain(int argc, char** argv) {
  g_consumer_cmd = new perfetto::PerfettoCmd();
  return g_consumer_cmd->Main(argc, argv);
}

}  // namespace perfetto
