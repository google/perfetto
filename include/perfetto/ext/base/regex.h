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

#ifndef INCLUDE_PERFETTO_EXT_BASE_REGEX_H_
#define INCLUDE_PERFETTO_EXT_BASE_REGEX_H_

#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/status_or.h"

#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
struct pcre2_real_code_8;
typedef struct pcre2_real_code_8 pcre2_code;
#elif PERFETTO_BUILDFLAG(PERFETTO_RE2)
namespace re2 {
class RE2;
}
#endif

namespace perfetto {
namespace base {

class Regex {
 public:
  Regex();
  ~Regex();

  Regex(Regex&&) noexcept;
  Regex& operator=(Regex&&) noexcept;

  // Regex is copyable by re-compiling the pattern.
  Regex(const Regex&);
  Regex& operator=(const Regex&);

  static StatusOr<Regex> Create(const std::string& pattern);

  bool Search(std::string_view s) const;

  bool FullMatch(std::string_view s) const;

  // Returns true if the regex matches and fills |out| with the capturing
  // groups. out[0] is the whole match, out[1..N] are the sub-groups.
  bool Submatch(std::string_view s, std::vector<std::string_view>& out) const;

  std::string Replace(std::string_view s, std::string_view replacement) const;

  bool IsValid() const;

 private:
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
  pcre2_code* pcre2_code_ = nullptr;
  std::unique_ptr<std::regex> std_re_;
#elif PERFETTO_BUILDFLAG(PERFETTO_RE2)
  std::unique_ptr<re2::RE2> re2_re_;
#else
  std::unique_ptr<std::regex> std_re_;
#endif

  std::string pattern_;
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_REGEX_H_
