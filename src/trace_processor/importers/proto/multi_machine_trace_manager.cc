/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/multi_machine_trace_manager.h"
#include <memory>

#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/clock_converter.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/sched_event_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/default_modules.h"
#include "src/trace_processor/importers/proto/perf_sample_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/proto_trace_parser_impl.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

MultiMachineTraceManager::MultiMachineTraceManager(
    TraceProcessorContext* default_context)
    : default_context_(default_context) {
  PERFETTO_DCHECK(default_context && !default_context_->machine_id());
}
MultiMachineTraceManager::~MultiMachineTraceManager() = default;

std::unique_ptr<TraceProcessorContext> MultiMachineTraceManager::CreateContext(
    RawMachineId raw_machine_id) {
  TraceProcessorContext::InitArgs args{
      default_context_->config, default_context_->storage, raw_machine_id};
  auto ctx = std::make_unique<TraceProcessorContext>(args);

  // Register default and additional modules (if enabled).
  RegisterDefaultModules(ctx.get());
  // Register addtional modules through the registered function pointer.
  if (additional_modules_factory_)
    additional_modules_factory_(ctx.get());

  return ctx;
}

void MultiMachineTraceManager::EnableAdditionalModules(
    ProtoImporterModuleFactory factory) {
  additional_modules_factory_ = factory;
}

ProtoTraceReader* MultiMachineTraceManager::GetOrCreateReader(
    RawMachineId raw_machine_id) {
  auto* remote_ctx = remote_machine_contexts_.Find(raw_machine_id);
  if (remote_ctx)
    return remote_ctx->reader.get();

  auto context = CreateContext(raw_machine_id);
  // Share the sorter, but enable for the parser.
  context->sorter = default_context_->sorter;
  context->sorter->AddMachineContext(context.get());
  context->process_tracker->SetPidZeroIsUpidZeroIdleProcess();
  context->proto_trace_parser.reset(new ProtoTraceParserImpl(context.get()));

  auto new_reader = std::make_unique<ProtoTraceReader>(context.get());
  remote_machine_contexts_[raw_machine_id] =
      RemoteMachineContext{std::move(context), std::move(new_reader)};
  return remote_machine_contexts_[raw_machine_id].reader.get();
}

}  // namespace trace_processor
}  // namespace perfetto
