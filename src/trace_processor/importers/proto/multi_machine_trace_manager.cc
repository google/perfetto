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
#include <utility>

#include "perfetto/base/logging.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/proto_trace_parser_impl.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/sorter/trace_sorter.h"  // IWYU pragma: keep
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

MultiMachineTraceManager::MultiMachineTraceManager(
    TraceProcessorContext* default_context)
    : default_context_(default_context) {
  PERFETTO_DCHECK(default_context &&
                  !default_context_->machine_context->machine_id());
}
MultiMachineTraceManager::~MultiMachineTraceManager() = default;

// TODO(sashwinbalaji): Fix this
std::unique_ptr<TraceProcessorContext> MultiMachineTraceManager::CreateContext(
    RawMachineId raw_machine_id) {
  TraceProcessorContext::InitArgs args{
      default_context_->global_context->config,
      default_context_->global_context->storage, raw_machine_id};
  auto new_context = std::make_unique<TraceProcessorContext>(args);
  new_context->register_additional_proto_modules =
      default_context_->global_context->register_additional_proto_modules;

  // Set up shared member fields:
  // arg_set_id is a monotonically increasing ID.
  // Share |global_args_tracker| between contexts.
  new_context->global_args_tracker =
      default_context_->trace_context->global_args_tracker;
  // Share the sorter, but enable for the parser.
  new_context->sorter = default_context_->global_context->sorter;
  new_context->machine_context->process_tracker
      ->SetPidZeroIsUpidZeroIdleProcess();

  return new_context;
}

ProtoTraceReader* MultiMachineTraceManager::GetOrCreateReader(
    RawMachineId raw_machine_id) {
  auto* remote_ctx = remote_machine_contexts_.Find(raw_machine_id);
  if (remote_ctx)
    return remote_ctx->reader.get();

  auto new_context = CreateContext(raw_machine_id);

  auto new_reader = std::make_unique<ProtoTraceReader>(new_context.get());
  remote_machine_contexts_[raw_machine_id] =
      RemoteMachineContext{std::move(new_context), std::move(new_reader)};
  return remote_machine_contexts_[raw_machine_id].reader.get();
}

}  // namespace perfetto::trace_processor
