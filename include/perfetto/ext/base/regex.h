/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_EXT_BASE_REGEX_H_
#define INCLUDE_PERFETTO_EXT_BASE_REGEX_H_

#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
#include <re2/re2.h>
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <regex.h>
#endif

namespace perfetto::base {

constexpr bool IsRegexSupported() {
#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
  return true;
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return false;
#else
  return true;
#endif
}

// Implements regex parsing and regex search based on C library `regex.h`
// or RE2 if PERFETTO_RE2 is PERFETTO_BUILDFLAG.
class Regex {
 public:
#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
  Regex(Regex&&) = default;
  Regex& operator=(Regex&&) = default;
  Regex(const Regex&) = delete;
  Regex& operator=(const Regex&) = delete;
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  ~Regex() {
    if (regex_) {
      regfree(&regex_.value());
    }
  }
  Regex(const Regex&) = delete;
  Regex(Regex&& other) {
    regex_ = other.regex_;
    other.regex_ = std::nullopt;
  }
  Regex& operator=(Regex&& other) {
    this->~Regex();
    new (this) Regex(std::move(other));
    return *this;
  }
  Regex& operator=(const Regex&) = delete;
#endif

  // Parse regex pattern. Returns error if regex pattern is invalid.
  static base::StatusOr<Regex> Create(const char* pattern) {
#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
    auto re = std::make_unique<re2::RE2>(pattern);
    if (!re->ok()) {
      return base::ErrStatus("Regex pattern '%s' is malformed.", pattern);
    }
    return Regex(std::move(re));
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED)) {
      return base::ErrStatus("Regex pattern '%s' is malformed.", pattern);
    }
    return Regex(regex);
#else
    base::ignore_result(pattern);
    PERFETTO_FATAL("Windows regex is not supported.");
#endif
  }

  // Returns true if string matches the regex.
  bool Search(const char* s) const {
#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
    PERFETTO_CHECK(re_);
    return re2::RE2::PartialMatch(s, *re_);
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    PERFETTO_CHECK(regex_);
    return regexec(&regex_.value(), s, 0, nullptr, 0) == 0;
#else
    base::ignore_result(s);
    PERFETTO_FATAL("Windows regex is not supported.");
#endif
  }

  // Returns a vector of string views representing the matched groups.
  // The first element is the full match. Subsequent elements are parenthesized
  // subexpressions.
  // Returns nullopt if there is no match.
  void Submatch(const char* s, std::vector<std::string_view>& out) {
    out.clear();
#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
    PERFETTO_CHECK(re_);
    int nmatch = re_->NumberOfCapturingGroups() + 1;
    std::vector<re2::StringPiece> pieces(static_cast<size_t>(nmatch));

    // RE2::Match performs the search (similar to regexec).
    // UNANCHORED allows matching anywhere in the string.
    if (!re_->Match(s, 0, strlen(s), re2::RE2::UNANCHORED, pieces.data(),
                    nmatch)) {
      return;
    }

    for (const auto& p : pieces) {
      if (p.data() == nullptr) {
        // Optional group that did not match.
        out.emplace_back();
      } else {
        out.emplace_back(p.data(), p.size());
      }
    }
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    PERFETTO_CHECK(regex_);
    const auto& rgx = regex_.value();
    size_t nmatch = rgx.re_nsub + 1;
    pmatch_.resize(nmatch);

    if (regexec(&rgx, s, nmatch, pmatch_.data(), 0) != 0) {
      return;
    }
    for (size_t i = 0; i < nmatch; ++i) {
      if (pmatch_[i].rm_so == -1) {
        // Optional group that did not match.
        out.emplace_back();
      } else {
        out.emplace_back(
            s + pmatch_[i].rm_so,
            static_cast<size_t>(pmatch_[i].rm_eo - pmatch_[i].rm_so));
      }
    }
#else
    base::ignore_result(out);
    if (s)
      PERFETTO_FATAL("Windows regex is not supported.");
#endif
  }

 private:
#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
  explicit Regex(std::unique_ptr<re2::RE2> re) : re_(std::move(re)) {}
  std::unique_ptr<re2::RE2> re_;
#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  explicit Regex(regex_t regex) : regex_(regex) {}

  std::optional<regex_t> regex_;
  std::vector<regmatch_t> pmatch_;
#endif
};

}  // namespace perfetto::base

#endif  // INCLUDE_PERFETTO_EXT_BASE_REGEX_H_
