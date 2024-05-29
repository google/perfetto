/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/winscope/shell_transitions_parser.h"
#include "src/trace_processor/importers/proto/winscope/shell_transitions_tracker.h"

#include "protos/perfetto/trace/android/shell_transition.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/proto/args_parser.h"
#include "src/trace_processor/importers/proto/winscope/winscope.descriptor.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

ShellTransitionsParser::ShellTransitionsParser(TraceProcessorContext* context)
    : context_(context), args_parser_{pool_} {
  pool_.AddFromFileDescriptorSet(kWinscopeDescriptor.data(),
                                 kWinscopeDescriptor.size());
}

void ShellTransitionsParser::ParseTransition(protozero::ConstBytes blob) {
  protos::pbzero::ShellTransition::Decoder transition(blob);

  auto row_id =
      ShellTransitionsTracker::GetOrCreate(context_)->InternTransition(
          transition.id());

  auto* window_manager_shell_transitions_table =
      context_->storage->mutable_window_manager_shell_transitions_table();
  auto row = window_manager_shell_transitions_table->FindById(row_id).value();

  if (transition.has_dispatch_time_ns()) {
    row.set_ts(transition.dispatch_time_ns());
  }

  auto inserter = context_->args_tracker->AddArgsTo(row_id);
  ArgsParser writer(/*timestamp=*/0, inserter, *context_->storage.get());
  base::Status status = args_parser_.ParseMessage(
      blob, kShellTransitionsProtoName, nullptr /* parse all fields */, writer);

  if (!status.ok()) {
    context_->storage->IncrementStats(
        stats::winscope_shell_transitions_parse_errors);
  }
}

void ShellTransitionsParser::ParseHandlerMappings(protozero::ConstBytes blob) {
  auto* shell_handlers_table =
      context_->storage
          ->mutable_window_manager_shell_transition_handlers_table();

  protos::pbzero::ShellHandlerMappings::Decoder handler_mappings(blob);
  for (auto it = handler_mappings.mapping(); it; ++it) {
    protos::pbzero::ShellHandlerMapping::Decoder mapping(it.field().as_bytes());

    tables::WindowManagerShellTransitionHandlersTable::Row row;
    row.handler_id = mapping.id();
    row.handler_name = context_->storage->InternString(
        base::StringView(mapping.name().ToStdString()));
    shell_handlers_table->Insert(row);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
