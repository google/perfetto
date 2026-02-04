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

#include "src/trace_processor/util/trace_enrichment/trace_enrichment.h"

#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/util/deobfuscation/deobfuscator.h"
#include "src/trace_processor/util/symbolizer/symbolize_database.h"

namespace perfetto::trace_processor::util {

namespace {

// Returns binary paths from mappings that might contain embedded symbols.
std::vector<std::string> GetSymbolFilesFromMappings(TraceProcessor* tp) {
  std::vector<std::string> files;
  auto it = tp->ExecuteQuery(R"(
    SELECT DISTINCT name
    FROM stack_profile_mapping
    WHERE build_id != '' AND name != ''
  )");
  while (it.Next()) {
    std::string name = it.Get(0).AsString();
    if (!name.empty() && name[0] == '/') {
      files.push_back(name);
    }
  }
  return files;
}

// Adds path to result if it exists.
void AddIfExists(std::vector<std::string>& result, const std::string& path) {
  if (!path.empty() && base::FileExists(path)) {
    result.push_back(path);
  }
}

// Discovers ProGuard/R8 mapping files in an Android Gradle project structure.
// Scans app/build/outputs/mapping/{buildVariant}/mapping.txt for all variants.
std::vector<std::string> DiscoverGradleMappings(
    const std::string& working_dir) {
  std::vector<std::string> mappings;

  std::string mapping_base = working_dir + "/app/build/outputs/mapping";
  if (!base::FileExists(mapping_base)) {
    return mappings;
  }

  std::vector<std::string> variants;
  if (!base::ListDirectories(mapping_base, variants).ok()) {
    return mappings;
  }

  for (const auto& variant : variants) {
    std::string mapping_file = mapping_base + "/" + variant + "/mapping.txt";
    if (base::FileExists(mapping_file)) {
      mappings.push_back(mapping_file);
    }
  }

  return mappings;
}

// Discovers native symbol paths from well-known locations.
std::vector<std::string> DiscoverSymbolPaths(
    const std::string& android_product_out,
    const std::string& working_dir,
    const std::string& home_dir,
    const std::string& root_dir) {
  std::vector<std::string> paths;

  // Default system debug directories.
  if (!root_dir.empty()) {
    AddIfExists(paths, root_dir + "/usr/lib/debug");
  }
  if (!home_dir.empty()) {
    AddIfExists(paths, home_dir + "/.debug");
  }

  // ANDROID_PRODUCT_OUT/symbols (AOSP builds).
  if (!android_product_out.empty()) {
    AddIfExists(paths, android_product_out + "/symbols");
  }

  // Gradle project paths (only if working_dir is provided).
  if (!working_dir.empty()) {
    // Gradle CMake output.
    AddIfExists(paths, working_dir + "/app/build/intermediates/cmake");

    // Gradle merged native libs.
    AddIfExists(paths,
                working_dir + "/app/build/intermediates/merged_native_libs");

    // Local .build-id cache.
    AddIfExists(paths, working_dir + "/.build-id");
  }

  return paths;
}

}  // namespace

EnrichmentResult EnrichTrace(TraceProcessor* tp,
                             const EnrichmentConfig& config) {
  EnrichmentResult result;
  bool symbolization_ok = false;
  bool deobfuscation_ok = false;

  const std::string& android_product_out = config.android_product_out;
  const std::string& home_dir = config.home_dir;
  const std::string& working_dir = config.working_dir;
  const std::string& root_dir = config.root_dir;

  // === Native Symbolization ===
  {
    profiling::SymbolizerConfig sym_config;

    // Start with explicit paths from config.
    sym_config.symbol_paths = config.symbol_paths;

    // Add discovered paths if auto-discovery is enabled.
    if (!config.no_auto_symbol_paths) {
      std::vector<std::string> discovered = DiscoverSymbolPaths(
          android_product_out, working_dir, home_dir, root_dir);
      for (const auto& path : discovered) {
        sym_config.symbol_paths.push_back(path);
      }
    }

    // Add binary paths from mappings (they might contain embedded symbols).
    sym_config.symbol_files = GetSymbolFilesFromMappings(tp);

    auto sym_result = profiling::SymbolizeDatabase(tp, sym_config);
    switch (sym_result.error) {
      case profiling::SymbolizerError::kOk:
        result.native_symbols = std::move(sym_result.symbols);
        symbolization_ok = true;
        break;
      case profiling::SymbolizerError::kSymbolizerNotAvailable:
        result.details +=
            "Symbolizer not available: " + sym_result.error_details + "\n";
        break;
      case profiling::SymbolizerError::kSymbolizationFailed:
        result.details +=
            "Symbolization failed: " + sym_result.error_details + "\n";
        break;
    }
  }

  // === Java Deobfuscation ===
  bool explicit_maps_failed = false;
  {
    // First, process explicit maps from config (these must succeed).
    std::vector<profiling::ProguardMap> explicit_maps;
    for (const auto& map_spec : config.proguard_maps) {
      explicit_maps.push_back({map_spec.package, map_spec.path});
    }

    if (!explicit_maps.empty()) {
      std::string explicit_data;
      bool success = profiling::ReadProguardMapsToDeobfuscationPackets(
          explicit_maps,
          [&explicit_data](std::string packet) { explicit_data += packet; });

      if (success) {
        result.deobfuscation_data = std::move(explicit_data);
        deobfuscation_ok = true;
      } else {
        result.details += "Failed to read ProGuard mapping files\n";
        explicit_maps_failed = true;
      }
    } else {
      deobfuscation_ok = true;
    }

    // Then, process discovered/auto maps (these can fail silently).
    if (!config.no_auto_proguard_maps && deobfuscation_ok) {
      std::vector<profiling::ProguardMap> auto_maps;

      // Add maps from PERFETTO_PROGUARD_MAP environment variable.
      std::vector<profiling::ProguardMap> env_maps =
          profiling::GetPerfettoProguardMapPath();
      for (const auto& m : env_maps) {
        auto_maps.push_back(m);
      }

      // Discover Gradle ProGuard maps.
      if (!working_dir.empty()) {
        std::vector<std::string> gradle_maps =
            DiscoverGradleMappings(working_dir);
        for (const auto& path : gradle_maps) {
          auto_maps.push_back({"", path});
        }
      }

      if (!auto_maps.empty()) {
        std::string auto_data;
        bool success = profiling::ReadProguardMapsToDeobfuscationPackets(
            auto_maps,
            [&auto_data](std::string packet) { auto_data += packet; });

        if (success) {
          result.deobfuscation_data += auto_data;
        }
        // Failure of auto-discovered maps is not a hard error.
      }
    }
  }

  // Determine overall status.
  if (explicit_maps_failed) {
    // Explicit user-provided maps must succeed.
    result.error = EnrichmentError::kExplicitMapsFailed;
  } else if (symbolization_ok && deobfuscation_ok) {
    result.error = EnrichmentError::kOk;
  } else if (symbolization_ok || deobfuscation_ok) {
    result.error = EnrichmentError::kPartialSuccess;
  } else {
    result.error = EnrichmentError::kAllFailed;
  }

  return result;
}

}  // namespace perfetto::trace_processor::util
