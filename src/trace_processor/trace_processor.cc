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

#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/table.h"
#include "src/trace_processor/trace_processor_impl.h"

namespace perfetto {
namespace trace_processor {

TraceProcessor::TraceProcessor(const Config& config)
    : impl_(
          std::unique_ptr<TraceProcessorImpl>(new TraceProcessorImpl(config))) {
}

TraceProcessor::~TraceProcessor() = default;

bool TraceProcessor::Parse(std::unique_ptr<uint8_t[]> data, size_t size) {
  return impl_->Parse(std::move(data), size);
}

void TraceProcessor::NotifyEndOfFile() {
  impl_->NotifyEndOfFile();
}

void TraceProcessor::ExecuteQuery(
    const protos::RawQueryArgs& args,
    std::function<void(const protos::RawQueryResult&)> callback) {
  impl_->ExecuteQuery(args, callback);
}

void TraceProcessor::InterruptQuery() {
  impl_->InterruptQuery();
}

// static
void EnableSQLiteVtableDebugging() {
  // This level of indirection is required to avoid clients to depend on table.h
  // which in turn requires sqlite headers.
  Table::debug = true;
}
}  // namespace trace_processor
}  // namespace perfetto
