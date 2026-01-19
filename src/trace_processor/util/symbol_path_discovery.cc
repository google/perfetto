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

#include "src/trace_processor/util/symbol_path_discovery.h"

#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"

namespace perfetto::trace_processor::util {

namespace {

// Checks if a directory exists.
bool DirectoryExists(const std::string& path) {
  return base::FileExists(path);
}

// Adds path to result if it exists.
void AddIfExists(std::vector<std::string>& result, const std::string& path) {
  if (!path.empty() && DirectoryExists(path)) {
    result.push_back(path);
  }
}

}  // namespace

DiscoveredPaths DiscoverSymbolPaths(
    const std::vector<std::string>& explicit_paths,
    const std::string& android_product_out,
    const std::string& working_dir) {
  DiscoveredPaths result;

  // 1. Explicit paths have highest priority (already validated by caller).
  for (const auto& path : explicit_paths) {
    if (!path.empty()) {
      result.native_symbol_paths.push_back(path);
    }
  }

  // 2. ANDROID_PRODUCT_OUT/symbols (AOSP builds).
  if (!android_product_out.empty()) {
    AddIfExists(result.native_symbol_paths, android_product_out + "/symbols");
  }

  // 3-5. Gradle project paths (only if working_dir is provided).
  if (!working_dir.empty()) {
    // Gradle CMake output.
    AddIfExists(result.native_symbol_paths,
                working_dir + "/app/build/intermediates/cmake");

    // Gradle merged native libs.
    AddIfExists(result.native_symbol_paths,
                working_dir + "/app/build/intermediates/merged_native_libs");

    // Local .build-id cache.
    AddIfExists(result.native_symbol_paths, working_dir + "/.build-id");

    // ProGuard/R8 mapping files from Gradle builds.
    // Common locations: app/build/outputs/mapping/{variant}/mapping.txt
    std::string mapping_base = working_dir + "/app/build/outputs/mapping";
    if (DirectoryExists(mapping_base)) {
      // Check common variants.
      for (const char* variant : {"release", "debug"}) {
        std::string mapping_file =
            mapping_base + "/" + variant + "/mapping.txt";
        if (base::FileExists(mapping_file)) {
          result.proguard_map_paths.push_back(mapping_file);
        }
      }
    }
  }

  return result;
}

}  // namespace perfetto::trace_processor::util
