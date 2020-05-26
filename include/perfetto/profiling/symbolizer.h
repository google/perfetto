/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PROFILING_SYMBOLIZER_H_
#define INCLUDE_PERFETTO_PROFILING_SYMBOLIZER_H_

#include <map>
#include <string>
#include <vector>

// TODO(135923303): do not depend on anything in this file as it will be
// removed as part of fixing b/135923303.
namespace perfetto {
namespace trace_to_text {

struct SymbolizedFrame {
  std::string function_name;
  std::string file_name;
  uint32_t line;
};

class Symbolizer {
 public:
  // For each address in the input vector, output a vector of SymbolizedFrame
  // representing the functions corresponding to that address. When inlining
  // occurs, this can be more than one function for a single address.
  //
  // On failure, return an empty vector.
  virtual std::vector<std::vector<SymbolizedFrame>> Symbolize(
      const std::string& mapping_name,
      const std::string& build_id,
      const std::vector<uint64_t>& address) = 0;
  virtual ~Symbolizer();
};

}  // namespace trace_to_text
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_PROFILING_SYMBOLIZER_H_
