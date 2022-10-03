/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACED_PROBES_STATSD_CLIENT_STATSD_DATA_SOURCE_H_
#define SRC_TRACED_PROBES_STATSD_CLIENT_STATSD_DATA_SOURCE_H_

#include <array>
#include <limits>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "src/protozero/proto_ring_buffer.h"
#include "src/traced/probes/probes_data_source.h"

namespace perfetto {

namespace base {
class TaskRunner;
}  // namespace base

namespace protos {
namespace pbzero {
class ProcessTree;
class Statsd;
class Statsd_Process;
}  // namespace pbzero
}  // namespace protos

// We have two ways to talk to statsd:
// - via execing cmd
// - via binder:
// https://cs.android.com/android/platform/superproject/+/master:frameworks/native/libs/binder/ndk/include_cpp/android/binder_interface_utils.h;l=239?q=android%2Fbinder_interface_utils.h
// TODO(hjd): Implement binder backend.
class StatsdBackend {
 public:
  // output is a file descriptor that StatsdBackend will continously
  // write to until the backend is destoied. Normally ths would be the
  // 'write' side of a pipe.
  StatsdBackend(std::string input, base::ScopedFile output_wr);
  virtual ~StatsdBackend();

 protected:
  // Encoded ShellConfig which will be written to statsd stdin.
  std::string input_;
  // stdout file descriptor. Normally one end of a pipe.
  base::ScopedFile output_wr_;
};

class SizetPrefixedMessageReader final
    : public protozero::RingBufferMessageReader {
 public:
  SizetPrefixedMessageReader();
  virtual ~SizetPrefixedMessageReader() override;

 protected:
  virtual SizetPrefixedMessageReader::Message TryReadMessage(
      const uint8_t* start,
      const uint8_t* end) override;
};

class StatsdDataSource : public ProbesDataSource {
 public:
  static const ProbesDataSource::Descriptor descriptor;

  StatsdDataSource(base::TaskRunner*,
                   TracingSessionID,
                   std::unique_ptr<TraceWriter> writer,
                   const DataSourceConfig&);
  ~StatsdDataSource() override;

  base::WeakPtr<StatsdDataSource> GetWeakPtr() const;

  // ProbesDataSource implementation.
  void Start() override;
  void Flush(FlushRequestID, std::function<void()> callback) override;
  void ClearIncrementalState() override;

  // public for testing
  static std::string GenerateShellConfig(const DataSourceConfig& config);

 private:
  // Common functions.
  StatsdDataSource(const StatsdDataSource&) = delete;
  StatsdDataSource& operator=(const StatsdDataSource&) = delete;

  void OnStatsdWakeup();
  void DoRead();

  base::TaskRunner* const task_runner_;
  std::unique_ptr<TraceWriter> writer_;
  std::unique_ptr<StatsdBackend> backend_{};
  base::Pipe output_;
  std::string shell_subscription_;
  bool read_in_progress_ = false;
  SizetPrefixedMessageReader buffer_;

  base::WeakPtrFactory<StatsdDataSource> weak_factory_;  // Keep last.
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_STATSD_CLIENT_STATSD_DATA_SOURCE_H_
