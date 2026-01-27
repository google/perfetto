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

#ifndef INCLUDE_PERFETTO_EXT_BASE_REGEX_H_
#define INCLUDE_PERFETTO_EXT_BASE_REGEX_H_

#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
namespace re2 {
class RE2;
}
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <regex>
#endif

namespace perfetto {
namespace base {

// A simple wrapper around RE2 or std::regex.
// RE2 is used if PERFETTO_RE2 is enabled.
// std::regex is used if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) and RE2 is not
// enabled.
class Regex {
 public:
  enum class Option {
    kNone = 0,
    kCaseInsensitive = 1 << 0,
  };

  static constexpr bool IsRegexSupported() {
#if PERFETTO_BUILDFLAG(PERFETTO_RE2) || !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    return true;
#else
    return false;
#endif
  }

  static StatusOr<Regex> Create(const char* pattern,
                                Option opt = Option::kNone);

  explicit Regex(const std::string& pattern, Option opt = Option::kNone);
  ~Regex();

  // Move-only to avoid accidental copies of expensive regex objects.
  Regex(Regex&& other) noexcept;
  Regex& operator=(Regex&& other) noexcept;

  Regex(const Regex&) = delete;
  Regex& operator=(const Regex&) = delete;

  // Returns a copy of the regex.
  Regex Clone() const;

  // Returns true if the pattern was successfully compiled.
  bool IsValid() const;

  // Returns true if the full string matches the pattern.
  bool Match(const std::string& s) const;

  // Matches the pattern against the range [begin, end) exactly.
  // out_groups will be filled with the positions of submatches.
  // out_groups[i].first is a pointer to the start of the match,
  // out_groups[i].second is a pointer to the end of the match.
  // group 0 is the full match.
  bool Match(const char* begin,
             const char* end,
             std::vector<std::pair<char*, char*>>* out_groups = nullptr) const;

  // Returns true if the pattern matches a substring of s.
  bool Search(const std::string& s) const;

  // Searches for the pattern in s starting at offset.
  // out_pos and out_len will be set to the position and length of the full
  // match (group 0).
  // out_groups will be filled with submatches (group 1, 2, ...).
  bool Search(const std::string& s,
              size_t offset,
              size_t* out_pos,
              size_t* out_len,
              std::vector<std::string>* out_groups = nullptr) const;

  // Searches for the pattern in the range [begin, end) starting at offset.
  // out_groups will be filled with the positions of submatches (group 0, 1,
  // ...).
  bool Search(const char* begin,
              const char* end,
              size_t offset,
              std::vector<std::pair<char*, char*>>* out_groups) const;

  // Searches for the pattern in s and returns all submatches.
  // out[0] is the full match, out[1] is the first group, etc.
  // Returns true if a match was found.
  bool Extract(const std::string& s, std::vector<std::string>& out) const;

  // Returns a vector of string views representing the matched groups.
  // The first element is the full match. Subsequent elements are parenthesized
  // subexpressions.
  // Returns with empty |out| if there is no match.
  // This method keeps the captured strings alive until the next call to
  // Submatch or destruction.
  void Submatch(const char* s, std::vector<std::string_view>& out);

 private:
#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
  std::unique_ptr<re2::RE2> re_;
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  std::unique_ptr<std::regex> re_;
#endif
  std::vector<std::string> last_matches_;
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_REGEX_H_
