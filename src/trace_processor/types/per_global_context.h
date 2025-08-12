/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TYPES_PER_GLOBAL_CONTEXT_H_
#define SRC_TRACE_PROCESSOR_TYPES_PER_GLOBAL_CONTEXT_H_

#include <cstdint>
#include <memory>

#include "perfetto/trace_processor/basic_types.h"

namespace perfetto::trace_processor {

class ClockConverter;
class ClockTracker;
class DescriptorPool;
class MetadataTracker;
class MultiMachineTraceManager;
class TraceReaderRegistry;
class TraceSorter;
class TraceStorage;
struct TraceProcessorContext;
struct ProtoImporterModuleContext;

class PerGlobalContext {
 public:
  struct InitArgs {
    Config config;
    std::shared_ptr<TraceStorage> storage;
    uint32_t raw_machine_id = 0;
  };

  explicit PerGlobalContext(const InitArgs&);

  void Init(TraceProcessorContext* context);

  // The default constructor is used in testing.
  PerGlobalContext();
  ~PerGlobalContext();

  PerGlobalContext(PerGlobalContext&&);
  PerGlobalContext& operator=(PerGlobalContext&&);

  Config config;

  // |storage| is shared among multiple contexts in multi-machine tracing.
  std::shared_ptr<TraceStorage> storage;

  std::unique_ptr<TraceReaderRegistry> reader_registry;

  // The sorter is used to sort trace data by timestamp and is shared among
  // multiple machines.
  std::shared_ptr<TraceSorter> sorter;

  std::unique_ptr<ClockTracker> clock_tracker;
  std::unique_ptr<ClockConverter> clock_converter;

  // TODO(sashwinbalaji): Split this into per-trace, per-machine and global
  // metadata.
  std::unique_ptr<MetadataTracker> metadata_tracker;

  // This field contains the list of proto descriptors that can be used by
  // reflection-based parsers.
  std::unique_ptr<DescriptorPool> descriptor_pool_;

  // Manages the contexts for reading trace data emitted from remote machines.
  std::unique_ptr<MultiMachineTraceManager> multi_machine_trace_manager;

  // The registration function for additional proto modules.
  // This is populated by TraceProcessorImpl to allow for late registration of
  // modules.
  using RegisterAdditionalProtoModulesFn = void(ProtoImporterModuleContext*,
                                                TraceProcessorContext*);
  RegisterAdditionalProtoModulesFn* register_additional_proto_modules = nullptr;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_TYPES_PER_GLOBAL_CONTEXT_H_
