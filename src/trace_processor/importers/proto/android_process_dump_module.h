/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_PROCESS_DUMP_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_PROCESS_DUMP_MODULE_H_

#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace protozero {
struct ConstBytes;
}

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class AndroidProcessDumpModule : public ProtoImporterModule {
 public:
  explicit AndroidProcessDumpModule(ProtoImporterModuleContext* module_context,
                                    TraceProcessorContext* context);
  ~AndroidProcessDumpModule() override;

  void ParseField(const ParseFieldArgs& args) override;

 private:
  void ParseAndroidProcessState(int64_t ts, protozero::ConstBytes blob);
  void ParseAndroidFreezerState(int64_t ts, protozero::ConstBytes blob);

  TraceProcessorContext* context_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_PROCESS_DUMP_MODULE_H_
