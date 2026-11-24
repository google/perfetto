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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_WATTSON_WATTSON_SOC_MODEL_MODULE_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_WATTSON_WATTSON_SOC_MODEL_MODULE_H_

#include <memory>
#include <vector>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"

namespace perfetto::trace_processor {
struct PluginDataframe;
class StringPool;
class TraceProcessorContext;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::wattson {

struct WattsonExternalTables;

struct WattsonExternalTablesDeleter {
  void operator()(WattsonExternalTables* tables) const;
};
using WattsonExternalTablesPtr =
    std::unique_ptr<WattsonExternalTables, WattsonExternalTablesDeleter>;

WattsonExternalTablesPtr CreateWattsonExternalTables(StringPool* pool);
void RegisterWattsonExternalDataframes(WattsonExternalTables* tables,
                                       std::vector<PluginDataframe>& out);

class WattsonSocModelModule : public ProtoImporterModule {
 public:
  WattsonSocModelModule(ProtoImporterModuleContext* module_context,
                        TraceProcessorContext* context,
                        WattsonExternalTables* tables);
  ~WattsonSocModelModule() override;

  void ParseField(const ParseFieldArgs& args) override;

 private:
  void ParseWattsonSocModel(protozero::ConstBytes blob);
  void ParseCpuCluster(protozero::ConstBytes blob);
  void ParseIdleStateMap(protozero::ConstBytes blob);
  void ParseSubsystem(protozero::ConstBytes blob);
  void ParseCpuCurve(protozero::ConstBytes blob);
  void ParseGpuCurve(protozero::ConstBytes blob);
  void ParseTpuCurve(protozero::ConstBytes blob);

  TraceProcessorContext* const context_;
  WattsonExternalTables* const tables_;
};

}  // namespace perfetto::trace_processor::wattson

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_WATTSON_WATTSON_SOC_MODEL_MODULE_H_
