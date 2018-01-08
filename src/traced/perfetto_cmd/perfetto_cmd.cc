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

#include <getopt.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/unix_task_runner.h"
#include "perfetto/base/utils.h"
#include "perfetto/traced/traced.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/trace_packet.h"
#include "perfetto/tracing/ipc/consumer_ipc_client.h"

#include "protos/tracing_service/trace_config.pb.h"

// TODO(primiano): add the ability to pass the file descriptor directly to the
// traced service instead of receiving a copy of the chunks and writing them
// from this process.

namespace perfetto {

class PerfettoCmd : public Consumer {
 public:
  int Main(int argc, char** argv);

  int PrintUsage(const char* argv0);
  void OnStopTraceTimer();

  // perfetto::Consumer implementation.
  void OnConnect() override;
  void OnDisconnect() override;
  void OnTraceData(std::vector<TracePacket>, bool has_more) override;

 private:
  perfetto::base::UnixTaskRunner task_runner_;
  std::unique_ptr<perfetto::Service::ConsumerEndpoint> consumer_endpoint_;
  std::unique_ptr<TraceConfig> trace_config_;
  std::ofstream trace_out_stream_;
  std::string trace_out_path_;
  bool did_receive_full_trace_ = false;
};

int PerfettoCmd::PrintUsage(const char* argv0) {
  fprintf(stderr, R"(Usage: %s
  --config  -c : /path/to/trace/config/file or - for stdin
  --out     -o : /path/to/out/trace/file
  --help
)", argv0);
  return 1;
}

int PerfettoCmd::Main(int argc, char** argv) {
  static const struct option long_options[] = {
      // |option_index| relies on the order of options, don't reshuffle them.
      {"help", required_argument, 0, 'h'},
      {"config", required_argument, 0, 'c'},
      {"out", required_argument, 0, 'o'},
      {0, 0, 0, 0}};

  int option_index = 0;
  std::string trace_config_raw;
  for (;;) {
    int option = getopt_long(argc, argv, "c:o:", long_options, &option_index);

    if (option == -1)
      break;  // EOF.

    if (option == 'c') {
      if (strcmp(optarg, "-") == 0) {
        std::istreambuf_iterator<char> begin(std::cin), end;
        trace_config_raw.assign(begin, end);
      } else if (strcmp(optarg, ":test") == 0) {
        // TODO(primiano): temporary for testing only.
        perfetto::protos::TraceConfig test_config;
        test_config.add_buffers()->set_size_kb(4096 * 10);
        test_config.set_duration_ms(3000);
        auto* ds_config = test_config.add_data_sources()->mutable_config();
        ds_config->set_name("perfetto.test");
        ds_config->set_target_buffer(0);
        ds_config->set_trace_category_filters("foo,bar");
        test_config.SerializeToString(&trace_config_raw);
      } else {
        std::ifstream file_stream;
        file_stream.open(optarg, std::ios_base::in | std::ios_base::binary);
        if (!file_stream.is_open()) {
          PERFETTO_ELOG("Could not open %s", optarg);
          return 1;
        }
        std::istreambuf_iterator<char> begin(file_stream), end;
        trace_config_raw.assign(begin, end);
      }
      continue;
    }

    if (option == 'o') {
      trace_out_path_ = optarg;
      trace_out_stream_.open(trace_out_path_ + ".tmp",
                             std::ios_base::out | std::ios_base::binary);
      if (!trace_out_stream_.is_open()) {
        PERFETTO_ELOG("Could not open %s", trace_out_path_.c_str());
        return 1;
      }
      continue;
    }

    return PrintUsage(argv[0]);
  }

  if (trace_config_raw.empty())
    return PrintUsage(argv[0]);
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

  consumer_endpoint_ = ConsumerIPCClient::Connect(PERFETTO_CONSUMER_SOCK_NAME,
                                                  this, &task_runner_);

  task_runner_.Run();
  return did_receive_full_trace_ ? 0 : 1;
}  // namespace perfetto

void PerfettoCmd::OnConnect() {
  PERFETTO_LOG(
      "Connected to the Perfetto traced service, starting tracing for %d ms",
      trace_config_->duration_ms());
  PERFETTO_DCHECK(trace_config_);
  consumer_endpoint_->EnableTracing(*trace_config_);
  task_runner_.PostDelayedTask(std::bind(&PerfettoCmd::OnStopTraceTimer, this),
                               trace_config_->duration_ms());
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

void PerfettoCmd::OnTraceData(std::vector<TracePacket> packets, bool has_more) {
  PERFETTO_LOG("Received packet %d", has_more);
  for (TracePacket& packet : packets) {
    for (const Chunk& chunk : packet) {
      trace_out_stream_.write(reinterpret_cast<const char*>(chunk.start),
                              chunk.size);
    }
  }
  if (has_more)
    return;

  // Reached end of trace.
  long bytes_written = trace_out_stream_.tellp();
  trace_out_stream_.close();
  std::string tmp_path = trace_out_path_ + ".tmp";
  PERFETTO_CHECK(rename(tmp_path.c_str(), trace_out_path_.c_str()) == 0);
  did_receive_full_trace_ = true;
  PERFETTO_ILOG("Wrote %ld bytes into %s", bytes_written,
                trace_out_path_.c_str());
  consumer_endpoint_->FreeBuffers();
  task_runner_.Quit();
}

int __attribute__((visibility("default")))
PerfettoCmdMain(int argc, char** argv) {
  perfetto::PerfettoCmd consumer_cmd;
  return consumer_cmd.Main(argc, argv);
}

}  // namespace perfetto
