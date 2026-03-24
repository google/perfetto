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

#include "perfetto/ext/base/regex.h"

#include <string_view>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flags.h"

#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#elif PERFETTO_BUILDFLAG(PERFETTO_RE2)
#include <re2/re2.h>
#endif

namespace perfetto {
namespace base {

Regex::Regex() = default;

Regex::~Regex() {
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
  if (pcre2_code_) {
    pcre2_code_free(pcre2_code_);
  }
#endif
}

Regex::Regex(Regex&& other) noexcept {
  *this = std::move(other);
}

Regex& Regex::operator=(Regex&& other) noexcept {
  if (this != &other) {
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
    if (pcre2_code_)
      pcre2_code_free(pcre2_code_);
    pcre2_code_ = other.pcre2_code_;
    other.pcre2_code_ = nullptr;
    std_re_ = std::move(other.std_re_);
#elif PERFETTO_BUILDFLAG(PERFETTO_RE2)
    re2_re_ = std::move(other.re2_re_);
#else
    std_re_ = std::move(other.std_re_);
#endif
    pattern_ = std::move(other.pattern_);
  }
  return *this;
}

Regex::Regex(const Regex& other) {
  if (!other.pattern_.empty()) {
    auto re_or = Regex::Create(other.pattern_);
    if (re_or.ok()) {
      *this = std::move(re_or.value());
    }
  }
}

Regex& Regex::operator=(const Regex& other) {
  if (this != &other) {
    if (other.pattern_.empty()) {
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
      if (pcre2_code_)
        pcre2_code_free(pcre2_code_);
      pcre2_code_ = nullptr;
      std_re_.reset();
#elif PERFETTO_BUILDFLAG(PERFETTO_RE2)
      re2_re_.reset();
#else
      std_re_.reset();
#endif
      pattern_.clear();
    } else {
      auto re_or = Regex::Create(other.pattern_);
      if (re_or.ok()) {
        *this = std::move(re_or.value());
      }
    }
  }
  return *this;
}

StatusOr<Regex> Regex::Create(const std::string& pattern) {
  Regex regex;
  regex.pattern_ = pattern;

#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
  if (PERFETTO_FLAGS(USE_PCRE2)) {
    int error_code;
    size_t error_offset;
    regex.pcre2_code_ = pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pattern.c_str()), PCRE2_ZERO_TERMINATED,
        PCRE2_MULTILINE, &error_code, &error_offset, nullptr);
    if (!regex.pcre2_code_) {
      PCRE2_UCHAR buffer[256];
      pcre2_get_error_message(error_code, buffer, sizeof(buffer));
      return ErrStatus("PCRE2 compile error at offset %zu: %s", error_offset,
                       reinterpret_cast<char*>(buffer));
    }
    return std::move(regex);
  }
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
  re2::RE2::Options options;
  options.set_log_errors(false);
  regex.re2_re_ = std::make_unique<re2::RE2>(pattern, options);
  if (!regex.re2_re_->ok()) {
    return ErrStatus("RE2 compile error: %s", regex.re2_re_->error().c_str());
  }
  return std::move(regex);
#else
  try {
    regex.std_re_ = std::make_unique<std::regex>(pattern, std::regex::extended);
  } catch (const std::regex_error& e) {
    return ErrStatus("std::regex compile error: %s", e.what());
  }
  return std::move(regex);
#endif
}

bool Regex::Search(std::string_view s) const {
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
  if (pcre2_code_) {
    pcre2_match_data* match_data =
        pcre2_match_data_create_from_pattern(pcre2_code_, nullptr);
    int rc = pcre2_match(pcre2_code_, reinterpret_cast<PCRE2_SPTR>(s.data()),
                         s.size(), 0, 0, match_data, nullptr);
    pcre2_match_data_free(match_data);
    return rc >= 0;
  }
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
  if (re2_re_) {
    return re2::RE2::PartialMatch(s, *re2_re_);
  }
#endif

#if !PERFETTO_BUILDFLAG(PERFETTO_RE2)
  if (std_re_) {
    return std::regex_search(s.begin(), s.end(), *std_re_);
  }
#endif

  return false;
}

bool Regex::FullMatch(std::string_view s) const {
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
  if (pcre2_code_) {
    pcre2_match_data* match_data =
        pcre2_match_data_create_from_pattern(pcre2_code_, nullptr);
    int rc = pcre2_match(pcre2_code_, reinterpret_cast<PCRE2_SPTR>(s.data()),
                         s.size(), 0, PCRE2_ANCHORED | PCRE2_ENDANCHORED,
                         match_data, nullptr);
    pcre2_match_data_free(match_data);
    return rc >= 0;
  }
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
  if (re2_re_) {
    return re2::RE2::FullMatch(s, *re2_re_);
  }
#endif

#if !PERFETTO_BUILDFLAG(PERFETTO_RE2)
  if (std_re_) {
    return std::regex_match(s.begin(), s.end(), *std_re_);
  }
#endif

  return false;
}

bool Regex::Submatch(std::string_view s,
                     std::vector<std::string_view>& out) const {
  out.clear();
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
  if (pcre2_code_) {
    pcre2_match_data* match_data =
        pcre2_match_data_create_from_pattern(pcre2_code_, nullptr);
    int rc = pcre2_match(pcre2_code_, reinterpret_cast<PCRE2_SPTR>(s.data()),
                         s.size(), 0, 0, match_data, nullptr);
    if (rc <= 0) {
      pcre2_match_data_free(match_data);
      return false;
    }
    size_t* ovector = pcre2_get_ovector_pointer(match_data);
    for (int i = 0; i < rc; ++i) {
      if (ovector[2 * i] == PCRE2_UNSET) {
        out.emplace_back();
      } else {
        out.emplace_back(s.data() + ovector[2 * i],
                         ovector[2 * i + 1] - ovector[2 * i]);
      }
    }
    pcre2_match_data_free(match_data);
    return true;
  }
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
  if (re2_re_) {
    int n = re2_re_->NumberOfCapturingGroups() + 1;
    std::vector<re2::StringPiece> groups(static_cast<size_t>(n));
    if (re2_re_->Match(s, 0, s.size(), re2::RE2::UNANCHORED, groups.data(),
                       n)) {
      for (const auto& gp : groups) {
        out.emplace_back(gp.data(), gp.size());
      }
      return true;
    }
    return false;
  }
#endif

#if !PERFETTO_BUILDFLAG(PERFETTO_RE2)
  if (std_re_) {
    std::cmatch m;
    if (std::regex_search(s.data(), s.data() + s.size(), m, *std_re_)) {
      for (size_t i = 0; i < m.size(); ++i) {
        out.emplace_back(s.data() + m.position(i),
                         static_cast<size_t>(m.length(i)));
      }
      return true;
    }
  }
#endif

  return false;
}

std::string Regex::Replace(std::string_view s,
                           std::string_view replacement) const {
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
  if (pcre2_code_) {
    std::string out;
    size_t out_len = s.size() + replacement.size() * 2 + 64;
    out.resize(out_len);
    int rc = pcre2_substitute(
        pcre2_code_, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(), 0,
        PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_EXTENDED, nullptr, nullptr,
        reinterpret_cast<PCRE2_SPTR>(replacement.data()), replacement.size(),
        reinterpret_cast<PCRE2_UCHAR*>(&out[0]), &out_len);
    if (rc >= 0) {
      out.resize(out_len);
      return out;
    }
    if (rc == PCRE2_ERROR_NOMEMORY) {
      out.resize(out_len);
      rc = pcre2_substitute(
          pcre2_code_, reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(), 0,
          PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_EXTENDED, nullptr, nullptr,
          reinterpret_cast<PCRE2_SPTR>(replacement.data()), replacement.size(),
          reinterpret_cast<PCRE2_UCHAR*>(&out[0]), &out_len);
      if (rc >= 0) {
        out.resize(out_len);
        return out;
      }
    }
    return std::string(s);
  }
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
  if (re2_re_) {
    std::string out(s);
    re2::RE2::GlobalReplace(&out, *re2_re_, replacement);
    return out;
  }
#endif

#if !PERFETTO_BUILDFLAG(PERFETTO_RE2)
  if (std_re_) {
    return std::regex_replace(std::string(s), *std_re_,
                              std::string(replacement));
  }
#endif

  return std::string(s);
}

bool Regex::IsValid() const {
#if PERFETTO_BUILDFLAG(PERFETTO_PCRE2)
  if (pcre2_code_)
    return true;
  return !!std_re_;
#elif PERFETTO_BUILDFLAG(PERFETTO_RE2)
  return !!re2_re_;
#else
  return !!std_re_;
#endif
}

}  // namespace base
}  // namespace perfetto
