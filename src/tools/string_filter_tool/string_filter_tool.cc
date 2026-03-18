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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/string_utils.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "src/proto_utils/txt_to_pb.h"
#include "src/protozero/filtering/string_filter.h"

namespace perfetto {
namespace string_filter_tool {
namespace {

const char kUsage[] =
    R"USAGE(Usage: string_filter_tool -r <rules_textproto> [-t <semantic_type>] <string>

Applies the Perfetto string filtering algorithm to <string> using the rules
defined in <rules_textproto> and prints the result to stdout.

Arguments:
  -r --rules:          Path to a TraceConfig textproto file. Only the
                       trace_filter.string_filter_chain field is used.
  -t --semantic_type:  Semantic type to use (integer, default: 0 = UNSPECIFIED).
  <string>             The string to filter (positional argument).

The rules textproto file should contain a TraceConfig with trace_filter rules,
for example:

  trace_filter {
    string_filter_chain {
      rules {
        policy: SFP_MATCH_REDACT_GROUPS
        regex_pattern: "foo(bar)baz"
      }
      rules {
        policy: SFP_ATRACE_MATCH_REDACT_GROUPS
        regex_pattern: "B\\|\\d+\\|(secret_event)(.*)"
        atrace_payload_starts_with: "secret_event"
      }
    }
  }

Output:
  Prints the (possibly filtered) string to stdout, followed by a newline.
  Exit code 0 if the string was modified, 1 if it was not.
)USAGE";

using TraceFilter = protos::gen::TraceConfig::TraceFilter;
using StringFilterRule = TraceFilter::StringFilterRule;

std::optional<protozero::StringFilter::Policy> ConvertPolicy(
    TraceFilter::StringFilterPolicy policy) {
  switch (policy) {
    case TraceFilter::SFP_UNSPECIFIED:
      return std::nullopt;
    case TraceFilter::SFP_MATCH_REDACT_GROUPS:
      return protozero::StringFilter::Policy::kMatchRedactGroups;
    case TraceFilter::SFP_ATRACE_MATCH_REDACT_GROUPS:
      return protozero::StringFilter::Policy::kAtraceMatchRedactGroups;
    case TraceFilter::SFP_MATCH_BREAK:
      return protozero::StringFilter::Policy::kMatchBreak;
    case TraceFilter::SFP_ATRACE_MATCH_BREAK:
      return protozero::StringFilter::Policy::kAtraceMatchBreak;
    case TraceFilter::SFP_ATRACE_REPEATED_SEARCH_REDACT_GROUPS:
      return protozero::StringFilter::Policy::kAtraceRepeatedSearchRedactGroups;
  }
  return std::nullopt;
}

protozero::StringFilter::SemanticTypeMask ConvertSemanticTypes(
    const StringFilterRule& rule) {
  protozero::StringFilter::SemanticTypeMask mask;
  if (rule.semantic_type().empty()) {
    mask.Set(0);
    return mask;
  }
  for (const auto& type : rule.semantic_type()) {
    auto semantic_type = static_cast<uint32_t>(type);
    if (semantic_type < protozero::StringFilter::SemanticTypeMask::kLimit) {
      mask.Set(semantic_type);
    }
  }
  return mask;
}

int Main(int argc, char** argv) {
  static const option long_options[] = {
      {"help", no_argument, nullptr, 'h'},
      {"rules", required_argument, nullptr, 'r'},
      {"semantic_type", required_argument, nullptr, 't'},
      {nullptr, 0, nullptr, 0}};

  std::string rules_path;
  uint32_t semantic_type = 0;

  for (;;) {
    int option = getopt_long(argc, argv, "hr:t:", long_options, nullptr);
    if (option == -1)
      break;

    if (option == 'h') {
      fprintf(stdout, "%s", kUsage);
      return 0;
    }

    if (option == 'r') {
      rules_path = optarg;
      continue;
    }

    if (option == 't') {
      auto parsed = base::CStringToUInt32(optarg);
      if (!parsed.has_value()) {
        fprintf(stderr, "Invalid semantic type: %s\n", optarg);
        return 1;
      }
      semantic_type = *parsed;
      continue;
    }

    fprintf(stderr, "%s", kUsage);
    return 1;
  }

  if (rules_path.empty() || optind >= argc) {
    fprintf(stderr, "%s", kUsage);
    return 1;
  }

  // The remaining positional argument is the string to filter.
  // Unescape C-style escape sequences (\n, \t, \\) so users can pass
  // strings containing newlines from the shell.
  std::string input_str;
  for (const char* p = argv[optind]; *p; ++p) {
    if (*p == '\\' && *(p + 1)) {
      switch (*(p + 1)) {
        case 'n':
          input_str += '\n';
          ++p;
          continue;
        case 't':
          input_str += '\t';
          ++p;
          continue;
        case '\\':
          input_str += '\\';
          ++p;
          continue;
        default:
          break;
      }
    }
    input_str += *p;
  }

  // Read and parse the rules textproto.
  std::string rules_data;
  if (!base::ReadFile(rules_path, &rules_data)) {
    PERFETTO_ELOG("Could not read rules file: %s", rules_path.c_str());
    return 1;
  }

  auto res = TraceConfigTxtToPb(rules_data, rules_path);
  if (!res.ok()) {
    fprintf(stderr, "%s\n", res.status().c_message());
    return 1;
  }

  std::vector<uint8_t>& config_bytes = res.value();
  protos::gen::TraceConfig config;
  config.ParseFromArray(config_bytes.data(), config_bytes.size());

  const auto& chain = config.trace_filter().string_filter_chain();

  protozero::StringFilter filter;
  for (const auto& rule : chain.rules()) {
    auto opt_policy = ConvertPolicy(rule.policy());
    if (!opt_policy) {
      PERFETTO_ELOG("Unknown string filter policy %d", rule.policy());
      return 1;
    }
    filter.AddRule(*opt_policy, rule.regex_pattern(),
                   rule.atrace_payload_starts_with(), rule.name(),
                   ConvertSemanticTypes(rule));
  }

  // Also load v54 chain if present.
  for (const auto& rule :
       config.trace_filter().string_filter_chain_v54().rules()) {
    auto opt_policy = ConvertPolicy(rule.policy());
    if (!opt_policy) {
      PERFETTO_ELOG("Unknown string filter policy %d", rule.policy());
      return 1;
    }
    filter.AddRule(*opt_policy, rule.regex_pattern(),
                   rule.atrace_payload_starts_with(), rule.name(),
                   ConvertSemanticTypes(rule));
  }

  // Apply the filter. MaybeFilter modifies the string in-place.
  bool was_modified =
      filter.MaybeFilter(input_str.data(), input_str.size(), semantic_type);

  // Print the result.
  printf("%s\n", input_str.c_str());
  return was_modified ? 0 : 1;
}

}  // namespace
}  // namespace string_filter_tool
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::string_filter_tool::Main(argc, argv);
}
