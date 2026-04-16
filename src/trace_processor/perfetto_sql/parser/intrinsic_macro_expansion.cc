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

#include "src/trace_processor/perfetto_sql/parser/intrinsic_macro_expansion.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/perfetto_sql/syntaqlite/syntaqlite_perfetto.h"

namespace perfetto::trace_processor::perfetto_sql {
namespace {

// Split a token like `(a, b, (c, d), e)` on top-level commas. Nested parens
// are respected; whitespace around each element is trimmed. Returns
// std::nullopt on malformed input (missing outer parens or unbalanced).
// `()` yields an empty vector rather than a single empty string.
std::optional<std::vector<std::string>> SplitParenList(std::string_view raw) {
  std::string_view s = base::TrimWhitespace(raw);
  if (s.size() < 2 || s.front() != '(' || s.back() != ')')
    return std::nullopt;
  s.remove_prefix(1);
  s.remove_suffix(1);
  std::vector<std::string> out;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '(') {
      ++depth;
    } else if (c == ')') {
      if (depth == 0)
        return std::nullopt;
      --depth;
    } else if (c == ',' && depth == 0) {
      out.emplace_back(base::TrimWhitespace(s.substr(start, i - start)));
      start = i + 1;
    }
  }
  if (depth != 0)
    return std::nullopt;
  out.emplace_back(base::TrimWhitespace(s.substr(start)));
  if (out.size() == 1 && out[0].empty())
    out.clear();
  return out;
}

// Builds the body of a token_apply invocation. Accepts either
// `apply!(macro, (a, b, c))` or `apply!(macro, (a, b), (c, d))`: emits
// `macro!(a)`/`macro!(a, c)` etc joined by `joiner` (and optionally
// prefixed with `joiner` when the result is non-empty).
std::optional<std::string> BuildApply(const SyntaqliteToken* args,
                                      uint32_t arg_count,
                                      std::string_view joiner,
                                      bool prefix) {
  if (arg_count != 2 && arg_count != 3)
    return std::nullopt;
  auto a = SplitParenList(std::string_view(args[1].text, args[1].length));
  if (!a)
    return std::nullopt;
  std::vector<std::string> b;
  if (arg_count == 3) {
    auto parsed =
        SplitParenList(std::string_view(args[2].text, args[2].length));
    if (!parsed)
      return std::nullopt;
    b = std::move(*parsed);
  }
  const auto& a_list = *a;
  size_t n = arg_count == 3 ? std::min(a_list.size(), b.size()) : a_list.size();
  std::string_view macro =
      base::TrimWhitespace(std::string_view(args[0].text, args[0].length));
  std::vector<std::string> calls;
  calls.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    std::string call = std::string(macro) + "!(" + a_list[i];
    if (arg_count == 3) {
      call.append(", ").append(b[i]);
    }
    call += ')';
    calls.push_back(std::move(call));
  }
  std::string joined = base::Join(calls, std::string(joiner));
  if (prefix && !joined.empty())
    joined = std::string(joiner) + joined;
  return joined;
}

}  // namespace

ExpandResult TryExpandIntrinsicMacro(std::string_view name,
                                     const SyntaqliteToken* args,
                                     uint32_t arg_count) {
  if (name == "__intrinsic_stringify" ||
      name == "__intrinsic_stringify_ignore_table") {
    if (arg_count != 1)
      return {ExpandResult::kExpansionFailed, {}};
    std::string_view arg(args[0].text, args[0].length);
    return {ExpandResult::kExpanded,
            "'" + std::string(base::TrimWhitespace(arg)) + "'"};
  }

  struct ApplyVariant {
    std::string_view name;
    std::string_view joiner;
    bool prefix;
  };
  static constexpr std::array<ApplyVariant, 4> kVariants{{
      {"__intrinsic_token_apply", ", ", false},
      {"__intrinsic_token_apply_prefix", ", ", true},
      {"__intrinsic_token_apply_and", " AND ", false},
      {"__intrinsic_token_apply_and_prefix", " AND ", true},
  }};
  for (const auto& v : kVariants) {
    if (name != v.name)
      continue;
    auto body = BuildApply(args, arg_count, v.joiner, v.prefix);
    if (!body)
      return {ExpandResult::kExpansionFailed, {}};
    return {ExpandResult::kExpanded, std::move(*body)};
  }
  return {ExpandResult::kNotIntrinsic, {}};
}

}  // namespace perfetto::trace_processor::perfetto_sql
