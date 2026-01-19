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

#ifndef SRC_TRACE_PROCESSOR_UTIL_SYMBOL_PATH_DISCOVERY_H_
#define SRC_TRACE_PROCESSOR_UTIL_SYMBOL_PATH_DISCOVERY_H_

#include <string>
#include <vector>

namespace perfetto::trace_processor::util {

// Discovered symbol paths from well-known locations.
struct DiscoveredPaths {
  // Paths to search for native symbol files (ELF with DWARF).
  std::vector<std::string> native_symbol_paths;

  // Paths to ProGuard/R8 mapping files (for commit 4).
  // Format: each entry is "package=path" or just "path".
  std::vector<std::string> proguard_map_paths;
};

// Discovers symbol paths from well-known locations.
//
// This function does NOT read environment variables directly - the caller
// should pass in values from env vars if desired. This makes the function
// easier to test and gives the caller control over env var handling.
//
// Discovery priority for native symbols:
// 1. explicit_paths (passed by caller, highest priority)
// 2. android_product_out + "/symbols" (AOSP builds)
// 3. working_dir + "/app/build/intermediates/cmake/*/obj" (Gradle CMake)
// 4. working_dir + "/app/build/intermediates/merged_native_libs" (Gradle)
// 5. working_dir + "/.build-id" (local debuginfod cache)
//
// Args:
//   explicit_paths: Paths explicitly provided by user (e.g., --symbol-path)
//   android_product_out: Value of ANDROID_PRODUCT_OUT env var, or empty
//   working_dir: Current working directory for Gradle project detection
//
// Returns:
//   DiscoveredPaths with all found symbol paths
DiscoveredPaths DiscoverSymbolPaths(
    const std::vector<std::string>& explicit_paths,
    const std::string& android_product_out,
    const std::string& working_dir);

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_SYMBOL_PATH_DISCOVERY_H_
