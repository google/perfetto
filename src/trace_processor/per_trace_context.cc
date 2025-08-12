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

#include "src/trace_processor/types/per_trace_context.h"

#include <memory>
#include <optional>

#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"

namespace perfetto::trace_processor {

PerTraceContext::PerTraceContext() = default;

PerTraceContext::Init() {}

PerTraceContext::~PerTraceContext() = default;

PerTraceContext::PerTraceContext(PerTraceContext&&) = default;
PerTraceContext& PerTraceContext::operator=(PerTraceContext&&) = default;

}  // namespace perfetto::trace_processor
