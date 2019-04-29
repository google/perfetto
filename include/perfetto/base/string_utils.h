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

#ifndef INCLUDE_PERFETTO_BASE_STRING_UTILS_H_
#define INCLUDE_PERFETTO_BASE_STRING_UTILS_H_

#include <string>
#include <vector>

namespace perfetto {
namespace base {

bool StartsWith(const std::string& str, const std::string& prefix);
bool EndsWith(const std::string& str, const std::string& suffix);
bool Contains(const std::string& haystack, const std::string& needle);
std::string Join(const std::vector<std::string>& parts,
                 const std::string& delim);
std::vector<std::string> SplitString(const std::string& text,
                                     const std::string& delimiter);

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_STRING_UTILS_H_
