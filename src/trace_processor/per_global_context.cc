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

#include "src/trace_processor/types/per_global_context.h"

#include <memory>
#include "src/trace_processor/importers/common/clock_converter.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/proto/multi_machine_trace_manager.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/trace_processor_storage_impl.h"
#include "src/trace_processor/trace_reader_registry.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto::trace_processor {

GlobalContext::GlobalContext(const InitArgs& args)
    : config(args.config), storage(args.storage) {
  metadata_tracker = std::make_unique<MetadataTracker>(storage.get());
  descriptor_pool_ = std::make_unique<DescriptorPool>();
}

GlobalContext::Init(TPContext* context) {
  reader_registry = std::make_unique<TraceReaderRegistry>(context);
  multi_machine_trace_manager =
      std::make_unique<MultiMachineTraceManager>(context);
  clock_tracker = std::make_unique<ClockTracker>(context);
  clock_converter = std::make_unique<ClockConverter>(context);
}

GlobalContext::GlobalContext() = default;
GlobalContext::~GlobalContext() = default;

GlobalContext::GlobalContext(GlobalContext&&) = default;
GlobalContext& GlobalContext::operator=(GlobalContext&&) = default;

}  // namespace perfetto::trace_processor
