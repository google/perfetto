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

// static
std::unique_ptr<TraceProcessor> TraceProcessor::CreateInstance(
    const Config& config) {
  return std::unique_ptr<TraceProcessor>(new TraceProcessorImpl(config));
}

TraceProcessor::~TraceProcessor() = default;

TraceProcessor::Iterator::Iterator(std::unique_ptr<IteratorImpl> iterator)
    : iterator_(std::move(iterator)) {}
TraceProcessor::Iterator::~Iterator() = default;

TraceProcessor::Iterator::Iterator(TraceProcessor::Iterator&&) noexcept =
    default;
TraceProcessor::Iterator& TraceProcessor::Iterator::operator=(
    TraceProcessor::Iterator&&) = default;

TraceProcessor::Iterator::NextResult TraceProcessor::Iterator::Next() {
  PERFETTO_DCHECK(IsValid());
  return iterator_->Next();
}

SqlValue TraceProcessor::Iterator::Get(uint32_t col) {
  PERFETTO_DCHECK(IsValid());
  return iterator_->Get(col);
}

uint32_t TraceProcessor::Iterator::ColumnCount() {
  PERFETTO_DCHECK(IsValid());
  return iterator_->ColumnCount();
}

base::Optional<std::string> TraceProcessor::Iterator::GetLastError() {
  PERFETTO_DCHECK(IsValid());
  return iterator_->GetLastError();
}

bool TraceProcessor::Iterator::IsValid() {
  return iterator_->IsValid();
}

// static
void EnableSQLiteVtableDebugging() {
  // This level of indirection is required to avoid clients to depend on table.h
  // which in turn requires sqlite headers.
  Table::debug = true;
}
}  // namespace trace_processor
}  // namespace perfetto
