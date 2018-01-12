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

#include "perfetto/ftrace_reader/ftrace_controller.h"
#include "perfetto/tracing/core/producer.h"
#include "perfetto/tracing/core/trace_writer.h"
#include "perfetto/tracing/ipc/producer_ipc_client.h"

#ifndef SRC_TRACED_PROBES_FTRACE_PRODUCER_H_
#define SRC_TRACED_PROBES_FTRACE_PRODUCER_H_

namespace perfetto {
class FtraceProducer : public Producer {
 public:
  ~FtraceProducer() override;

  // Producer Impl:
  void OnConnect() override;
  void OnDisconnect() override;
  void CreateDataSourceInstance(DataSourceInstanceID,
                                const DataSourceConfig&) override;
  void TearDownDataSourceInstance(DataSourceInstanceID) override;

  // Our Impl
  void Run();

 private:
  using BundleHandle =
      protozero::ProtoZeroMessageHandle<protos::pbzero::FtraceEventBundle>;

  class SinkDelegate : public FtraceSink::Delegate {
   public:
    SinkDelegate(std::unique_ptr<TraceWriter> writer);
    ~SinkDelegate() override;

    // FtraceDelegateImpl
    BundleHandle GetBundleForCpu(size_t cpu) override;
    void OnBundleComplete(size_t cpu, BundleHandle bundle) override;

    void sink(std::unique_ptr<FtraceSink> sink) { sink_ = std::move(sink); }

   private:
    std::unique_ptr<FtraceSink> sink_ = nullptr;
    TraceWriter::TracePacketHandle trace_packet_;
    std::unique_ptr<TraceWriter> writer_;
  };

  std::unique_ptr<Service::ProducerEndpoint> endpoint_ = nullptr;
  std::unique_ptr<FtraceController> ftrace_ = nullptr;
  DataSourceID data_source_id_ = 0;
  std::map<DataSourceInstanceID, std::unique_ptr<SinkDelegate>> delegates_;
};
}  // namespace perfetto

#endif
