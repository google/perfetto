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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WATTSON_SOC_MODEL_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WATTSON_SOC_MODEL_MODULE_H_

#include <cstdint>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

class WattsonSocModelModule : public ProtoImporterModule {
 public:
  WattsonSocModelModule(ProtoImporterModuleContext* module_context,
                        TraceProcessorContext* context);
  ~WattsonSocModelModule() override;

  void ParseField(const ParseFieldArgs& args) override;

 private:
  void ParseWattsonSocModel(protozero::ConstBytes blob);

  TraceProcessorContext* const context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WATTSON_SOC_MODEL_MODULE_H_
