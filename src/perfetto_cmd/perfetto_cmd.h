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

#ifndef SRC_PERFETTO_CMD_PERFETTO_CMD_H_
#define SRC_PERFETTO_CMD_PERFETTO_CMD_H_

#include <memory>
#include <string>
#include <vector>

#include <time.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/unix_task_runner.h"
#include "perfetto/tracing/core/consumer.h"
#include "perfetto/tracing/ipc/consumer_ipc_client.h"
#include "src/perfetto_cmd/rate_limiter.h"

#include "src/perfetto_cmd/perfetto_cmd_state.pb.h"

#if defined(PERFETTO_OS_ANDROID)
#include "perfetto/base/android_task_runner.h"
#endif  // defined(PERFETTO_OS_ANDROID)


namespace perfetto {

// Temporary directory for DropBox traces. Note that this is automatically
// created by the system by setting setprop persist.traced.enable=1.
extern const char* kTempDropBoxTraceDir;

#if defined(PERFETTO_OS_ANDROID)
using PlatformTaskRunner = base::AndroidTaskRunner;
#else
using PlatformTaskRunner = base::UnixTaskRunner;
#endif

class PerfettoCmd : public Consumer {
 public:
  int Main(int argc, char** argv);

  // perfetto::Consumer implementation.
  void OnConnect() override;
  void OnDisconnect() override;
  void OnTracingDisabled() override;
  void OnTraceData(std::vector<TracePacket>, bool has_more) override;

  int ctrl_c_pipe_wr() const { return *ctrl_c_pipe_wr_; }

 private:
  bool OpenOutputFile();
  void SetupCtrlCSignalHandler();
  void FinalizeTraceAndExit();
  int PrintUsage(const char* argv0);
  void OnTimeout();

  PlatformTaskRunner task_runner_;
  std::unique_ptr<perfetto::TracingService::ConsumerEndpoint>
      consumer_endpoint_;
  std::unique_ptr<TraceConfig> trace_config_;
  base::ScopedFstream trace_out_stream_;
  std::string trace_out_path_;
  base::ScopedFile ctrl_c_pipe_wr_;
  base::ScopedFile ctrl_c_pipe_rd_;
  std::string dropbox_tag_;
  bool did_process_full_trace_ = false;
  size_t bytes_uploaded_to_dropbox_ = 0;
};

}  // namespace perfetto

#endif  // SRC_PERFETTO_CMD_PERFETTO_CMD_H_
