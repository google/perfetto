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

#include "perfetto/ext/base/regex.h"

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)
#include "re2/re2.h"
#endif

namespace perfetto {
namespace base {

StatusOr<Regex> Regex::Create(const char* pattern, Option opt) {
  Regex re(pattern, opt);
  if (!re.IsValid()) {
    return base::ErrStatus("Regex pattern '%s' is malformed.", pattern);
  }
  return std::move(re);
}

#if PERFETTO_BUILDFLAG(PERFETTO_RE2)

Regex::Regex(const std::string& pattern, Option opt) {
  re2::RE2::Options re2_opt;
  if (static_cast<int>(opt) & static_cast<int>(Option::kCaseInsensitive)) {
    re2_opt.set_case_sensitive(false);
  }
  re_ = std::make_unique<re2::RE2>(pattern, re2_opt);
}

Regex::~Regex() = default;
Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;

Regex Regex::Clone() const {
  Regex copy("");
  if (re_) {
    copy.re_ = std::make_unique<re2::RE2>(re_->pattern(), re_->options());
  }
  return copy;
}

bool Regex::IsValid() const {
  return re_ && re_->ok();
}

bool Regex::Match(const std::string& s) const {
  if (!IsValid())
    return false;
  return re2::RE2::FullMatch(s, *re_);
}

bool Regex::Match(const char* begin,
                  const char* end,
                  std::vector<std::pair<char*, char*>>* out_groups) const {
  if (!IsValid() || begin > end)
    return false;

  int n_groups = re_->NumberOfCapturingGroups();
  if (n_groups < 0)
    return false;

  size_t num_to_capture = static_cast<size_t>(n_groups) + 1;
  std::vector<re2::StringPiece> matches(num_to_capture);

  re2::StringPiece input(begin, static_cast<size_t>(end - begin));
  if (!re_->Match(input, 0, input.size(), re2::RE2::ANCHOR_BOTH, matches.data(),
                  static_cast<int>(num_to_capture))) {
    return false;
  }

  if (out_groups) {
    out_groups->clear();
    for (size_t i = 0; i < num_to_capture; ++i) {
      if (matches[i].data() == nullptr) {
        out_groups->push_back({nullptr, nullptr});
      } else {
        char* m_begin = const_cast<char*>(matches[i].data());
        out_groups->push_back({m_begin, m_begin + matches[i].size()});
      }
    }
  }

  return true;
}

bool Regex::Search(const std::string& s) const {
  if (!IsValid())
    return false;
  return re2::RE2::PartialMatch(s, *re_);
}

bool Regex::Search(const std::string& s,
                   size_t offset,
                   size_t* out_pos,
                   size_t* out_len,
                   std::vector<std::string>* out_groups) const {
  if (!IsValid() || offset > s.size())
    return false;

  int n_groups = re_->NumberOfCapturingGroups();
  if (n_groups < 0)
    return false;

  size_t num_to_capture = static_cast<size_t>(n_groups) + 1;
  std::vector<re2::StringPiece> matches(num_to_capture);

  if (!re_->Match(s, offset, s.size(), re2::RE2::UNANCHORED, matches.data(),
                  static_cast<int>(num_to_capture))) {
    return false;
  }

  if (out_pos) {
    *out_pos = static_cast<size_t>(matches[0].data() - s.data());
  }
  if (out_len) {
    *out_len = matches[0].size();
  }

  if (out_groups) {
    out_groups->clear();
    for (size_t i = 1; i < num_to_capture; ++i) {
      out_groups->push_back(std::string(matches[i]));
    }
  }

  return true;
}

bool Regex::Search(const char* begin,
                   const char* end,
                   size_t offset,
                   std::vector<std::pair<char*, char*>>* out_groups) const {
  if (!IsValid() || begin > end || offset > static_cast<size_t>(end - begin))
    return false;

  int n_groups = re_->NumberOfCapturingGroups();
  if (n_groups < 0)
    return false;

  size_t num_to_capture = static_cast<size_t>(n_groups) + 1;
  std::vector<re2::StringPiece> matches(num_to_capture);

  re2::StringPiece input(begin, static_cast<size_t>(end - begin));
  if (!re_->Match(input, offset, input.size(), re2::RE2::UNANCHORED,
                  matches.data(), static_cast<int>(num_to_capture))) {
    return false;
  }

  if (out_groups) {
    out_groups->clear();
    for (size_t i = 0; i < num_to_capture; ++i) {
      if (matches[i].data() == nullptr) {
        out_groups->push_back({nullptr, nullptr});
      } else {
        char* m_begin = const_cast<char*>(matches[i].data());
        out_groups->push_back({m_begin, m_begin + matches[i].size()});
      }
    }
  }

  return true;
}

bool Regex::Extract(const std::string& s, std::vector<std::string>& out) const {
  out.clear();
  if (!IsValid())
    return false;

  int n_groups = re_->NumberOfCapturingGroups();
  if (n_groups < 0)
    return false;

  // RE2::Match takes n_groups + 1 to include the full match (group 0).
  size_t num_to_capture = static_cast<size_t>(n_groups) + 1;
  std::vector<re2::StringPiece> matches(num_to_capture);

  if (!re_->Match(s, 0, s.size(), re2::RE2::UNANCHORED, matches.data(),
                  static_cast<int>(num_to_capture))) {
    return false;
  }

  for (size_t i = 0; i < num_to_capture; ++i) {
    out.push_back(std::string(matches[i]));
  }
  return true;
}

#elif !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

Regex::Regex(const std::string& pattern, Option opt) {
  std::regex_constants::syntax_option_type re_opt = std::regex::ECMAScript;
  if (static_cast<int>(opt) & static_cast<int>(Option::kCaseInsensitive)) {
    re_opt |= std::regex::icase;
  }
  // Note: std::regex can throw an exception on invalid patterns.
  // Since Perfetto builds without exceptions, this will cause a crash.
  // This is acceptable because Regex is intended to be used with
  // trusted/static patterns in the fallback case.
  re_ = std::make_unique<std::regex>(pattern, re_opt);
}

Regex::~Regex() = default;
Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;

Regex Regex::Clone() const {
  Regex copy("");
  if (re_) {
    copy.re_ = std::make_unique<std::regex>(*re_);
  }
  return copy;
}

bool Regex::IsValid() const {
  return !!re_;
}

bool Regex::Match(const std::string& s) const {
  if (!IsValid())
    return false;
  return std::regex_match(s, *re_);
}

bool Regex::Match(const char* begin,
                  const char* end,
                  std::vector<std::pair<char*, char*>>* out_groups) const {
  if (!IsValid() || begin > end)
    return false;

  std::cmatch m;
  if (!std::regex_match(begin, end, m, *re_)) {
    return false;
  }

  if (out_groups) {
    out_groups->clear();
    for (size_t i = 0; i < m.size(); ++i) {
      if (!m[i].matched) {
        out_groups->push_back({nullptr, nullptr});
      } else {
        out_groups->push_back(
            {const_cast<char*>(m[i].first), const_cast<char*>(m[i].second)});
      }
    }
  }

  return true;
}

bool Regex::Search(const std::string& s) const {
  if (!IsValid())
    return false;
  return std::regex_search(s, *re_);
}

bool Regex::Search(const std::string& s,
                   size_t offset,
                   size_t* out_pos,
                   size_t* out_len,
                   std::vector<std::string>* out_groups) const {
  if (!IsValid() || offset > s.size())
    return false;

  std::smatch m;
  auto start = s.begin() + static_cast<ptrdiff_t>(offset);
  if (!std::regex_search(start, s.end(), m, *re_)) {
    return false;
  }

  if (out_pos) {
    *out_pos = offset + static_cast<size_t>(m.position(0));
  }
  if (out_len) {
    *out_len = static_cast<size_t>(m.length(0));
  }

  if (out_groups) {
    out_groups->clear();
    for (size_t i = 1; i < m.size(); ++i) {
      out_groups->push_back(m[i].str());
    }
  }

  return true;
}

bool Regex::Search(const char* begin,
                   const char* end,
                   size_t offset,
                   std::vector<std::pair<char*, char*>>* out_groups) const {
  if (!IsValid() || begin > end || offset > static_cast<size_t>(end - begin))
    return false;

  std::cmatch m;
  if (!std::regex_search(begin + offset, end, m, *re_)) {
    return false;
  }

  if (out_groups) {
    out_groups->clear();
    for (size_t i = 0; i < m.size(); ++i) {
      if (!m[i].matched) {
        out_groups->push_back({nullptr, nullptr});
      } else {
        out_groups->push_back(
            {const_cast<char*>(m[i].first), const_cast<char*>(m[i].second)});
      }
    }
  }

  return true;
}

bool Regex::Extract(const std::string& s, std::vector<std::string>& out) const {
  out.clear();
  if (!IsValid())
    return false;
  std::smatch m;
  if (!std::regex_search(s, m, *re_)) {
    return false;
  }
  for (size_t i = 0; i < m.size(); ++i) {
    out.push_back(m[i].str());
  }
  return true;
}

#else  // !PERFETTO_RE2 && PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

Regex::Regex(const std::string&, Option) {}
Regex::~Regex() = default;
Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;
Regex Regex::Clone() const {
  return Regex("");
}
bool Regex::IsValid() const {
  return false;
}
bool Regex::Match(const std::string&) const {
  return false;
}
bool Regex::Match(const char*,
                  const char*,
                  std::vector<std::pair<char*, char*>>*) const {
  return false;
}
bool Regex::Search(const std::string&) const {
  return false;
}
bool Regex::Search(const std::string&,
                   size_t,
                   size_t*,
                   size_t*,
                   std::vector<std::string>*) const {
  return false;
}
bool Regex::Search(const char*,
                   const char*,
                   size_t,
                   std::vector<std::pair<char*, char*>>*) const {
  return false;
}
bool Regex::Extract(const std::string&, std::vector<std::string>&) const {
  return false;
}

#endif

void Regex::Submatch(const char* s, std::vector<std::string_view>& out) {
  out.clear();
  if (!Extract(s, last_matches_)) {
    return;
  }
  for (const auto& m : last_matches_) {
    out.emplace_back(m);
  }
}

}  // namespace base
}  // namespace perfetto
