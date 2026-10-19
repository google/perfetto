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

#ifndef SRC_TRACE_PROCESSOR_TRACE_PARSER_IMPL_H_
#define SRC_TRACE_PROCESSOR_TRACE_PARSER_IMPL_H_

#include "perfetto/trace_processor/trace_parser.h"
#include "src/trace_processor/trace_processor_storage_impl.h"

namespace perfetto::trace_processor {
// Reuse the existing parser core while the legacy storage entrypoint migrates.
class TraceParserImpl : public TraceParser {
 public:
  TraceParserImpl(const TraceParserConfig&, Sinks);
  explicit TraceParserImpl(const TraceParserOptions&);
  ~TraceParserImpl() override;
  base::Status Parse(TraceBlobView) override;
  base::Status NotifyEndOfFile() override;

  // Internal diagnostics/tests only; absent from the public parser interface.
  TraceProcessorContext* context() { return core_.context(); }

 private:
  struct OutputState;
  static TraceParserOptions MakeOptions(const TraceParserConfig&, OutputState*);
  void InitializeImporters();
  base::Status CheckOutputStatus();
  std::unique_ptr<OutputState> output_;
  TraceProcessorStorageImpl core_;
  base::Status status_;
  bool finished_ = false;
};
}  // namespace perfetto::trace_processor
#endif  // SRC_TRACE_PROCESSOR_TRACE_PARSER_IMPL_H_
