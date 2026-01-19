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

#include "src/traceconv/trace_to_bundle.h"

#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/read_trace.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/profiling/deobfuscator.h"
#include "src/trace_processor/util/auto_symbolizer.h"
#include "src/trace_processor/util/symbol_path_discovery.h"
#include "src/trace_processor/util/tar_writer.h"

namespace perfetto::trace_to_text {

int TraceToBundle(const std::string& input_file_path,
                  const std::string& output_file_path,
                  const BundleContext& context) {
  auto tp = trace_processor::TraceProcessor::CreateInstance({});
  auto status = trace_processor::ReadTrace(tp.get(), input_file_path.c_str());
  if (!status.ok()) {
    PERFETTO_ELOG("Failed to read trace: %s", status.c_message());
    return 1;
  }

  // Add original trace file directly (memory efficient).
  trace_processor::util::TarWriter tar(output_file_path);
  auto add_trace_status =
      tar.AddFileFromPath("trace.perfetto", input_file_path);
  if (!add_trace_status.ok()) {
    PERFETTO_ELOG("Failed to add trace to TAR archive: %s",
                  add_trace_status.c_message());
    return 1;
  }

  // Discover symbol paths from well-known locations.
  trace_processor::util::SymbolizerConfig sym_config;
  sym_config.no_auto_symbol_paths = context.no_auto_symbol_paths;
  sym_config.symbol_paths = context.symbol_paths;

  // Add paths discovered from well-known locations (unless disabled).
  if (!context.no_auto_symbol_paths) {
    auto discovered = trace_processor::util::DiscoverSymbolPaths(
        {}, context.android_product_out, context.working_dir);
    for (const auto& path : discovered.native_symbol_paths) {
      sym_config.symbol_paths.push_back(path);
    }
  }

  // Symbolize the trace if possible.
  auto sym_result = trace_processor::util::Symbolize(tp.get(), sym_config);
  switch (sym_result.error) {
    case trace_processor::util::SymbolizerError::kOk:
      if (!sym_result.symbols.empty()) {
        auto add_symbols_status = tar.AddFile("symbols.pb", sym_result.symbols);
        if (!add_symbols_status.ok()) {
          PERFETTO_ELOG("Failed to add symbols to TAR archive: %s",
                        add_symbols_status.c_message());
          return 1;
        }
      }
      break;
    case trace_processor::util::SymbolizerError::kNoMappingsToSymbolize:
      // No mappings to symbolize is not an error.
      break;
    case trace_processor::util::SymbolizerError::kSymbolizerNotAvailable:
      PERFETTO_ELOG("Symbolizer not available: %s",
                    sym_result.error_details.c_str());
      // Continue without symbols rather than failing.
      break;
    case trace_processor::util::SymbolizerError::kSymbolizationFailed:
      PERFETTO_ELOG("Symbolization failed: %s",
                    sym_result.error_details.c_str());
      return 1;
  }

  // Collect ProGuard maps from explicit context and discovered paths.
  std::vector<profiling::ProguardMap> proguard_maps;
  for (const auto& map_spec : context.proguard_maps) {
    proguard_maps.push_back({map_spec.package, map_spec.path});
  }

  // Add discovered maps (unless auto-discovery is disabled).
  if (!context.no_auto_symbol_paths) {
    auto discovered = trace_processor::util::DiscoverSymbolPaths(
        {}, context.android_product_out, context.working_dir);
    for (const auto& path : discovered.proguard_map_paths) {
      proguard_maps.push_back({"", path});
    }
  }

  // Deobfuscate Java stack traces if ProGuard maps are available.
  if (!proguard_maps.empty()) {
    std::string deobfuscation_data;
    bool success = profiling::ReadProguardMapsToDeobfuscationPackets(
        proguard_maps,
        [&deobfuscation_data](std::string packet) {
          deobfuscation_data += packet;
        });

    if (!success) {
      PERFETTO_ELOG("Failed to read ProGuard mapping files");
      return 1;
    }

    if (!deobfuscation_data.empty()) {
      auto add_status = tar.AddFile("deobfuscation.pb", deobfuscation_data);
      if (!add_status.ok()) {
        PERFETTO_ELOG("Failed to add deobfuscation data to TAR: %s",
                      add_status.c_message());
        return 1;
      }
    }
  }

  return 0;
}

}  // namespace perfetto::trace_to_text
