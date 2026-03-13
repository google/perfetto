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

#include "src/trace_processor/shell/summarize_subcommand.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/trace_processor/trace_processor_shell.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/interactive.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/query.h"
#include "src/trace_processor/trace_summary/summary.h"

namespace perfetto::trace_processor::shell {

const char* SummarizeSubcommand::name() const {
  return "summarize";
}

const char* SummarizeSubcommand::description() const {
  return "Run trace summarization.";
}

void SummarizeSubcommand::PrintUsage(const char* argv0) {
  PERFETTO_ELOG(R"(
Run trace summarization.

Usage: %s summarize [flags] trace_file

Flags:
  --spec PATH            TraceSummarySpec proto path.
  --metrics-v2 IDS       Metric IDs, or "all".
  --metadata-query ID    Metadata query ID.
  --format [text|binary] Output format.
  --post-query FILE      SQL file to run after summarization.
)",
                argv0);
}

int SummarizeSubcommand::Run(const SubcommandContext& ctx,
                             int argc,
                             char** argv) {
  GlobalOptions global;
  std::string metrics_v2;
  std::string metadata_query;
  std::vector<std::string> spec_paths;
  std::string output_format;
  std::string post_query_path;
  bool interactive = false;

  enum LocalOption {
    OPT_SPEC = 500,
    OPT_METRICS_V2,
    OPT_METADATA_QUERY,
    OPT_FORMAT,
    OPT_POST_QUERY,
  };

  static const option long_options[] = {
      {"spec", required_argument, nullptr, OPT_SPEC},
      {"metrics-v2", required_argument, nullptr, OPT_METRICS_V2},
      {"metadata-query", required_argument, nullptr, OPT_METADATA_QUERY},
      {"format", required_argument, nullptr, OPT_FORMAT},
      {"post-query", required_argument, nullptr, OPT_POST_QUERY},
      {"interactive", no_argument, nullptr, 'i'},
      GLOBAL_LONG_OPTIONS{nullptr, 0, nullptr, 0}};

  optind = 1;
  for (;;) {
    int option = getopt_long(argc, argv, "im:h", long_options, nullptr);
    if (option == -1)
      break;
    if (HandleGlobalOption(option, optarg, global))
      continue;
    if (option == OPT_SPEC) {
      spec_paths.emplace_back(optarg);
      continue;
    }
    if (option == OPT_METRICS_V2) {
      metrics_v2 = optarg;
      continue;
    }
    if (option == OPT_METADATA_QUERY) {
      metadata_query = optarg;
      continue;
    }
    if (option == OPT_FORMAT) {
      output_format = optarg;
      continue;
    }
    if (option == OPT_POST_QUERY) {
      post_query_path = optarg;
      continue;
    }
    if (option == 'i') {
      interactive = true;
      continue;
    }
    PrintUsage(argv[0]);
    return option == 'h' ? 0 : 1;
  }

  if (optind == argc - 1 && argv[optind]) {
    global.trace_file = argv[optind];
  } else {
    PERFETTO_ELOG("summarize: trace file is required");
    return 1;
  }

  auto config = BuildConfig(global, ctx.platform);
  auto tp_or = SetupTraceProcessor(global, config, ctx.platform);
  if (!tp_or.ok()) {
    PERFETTO_ELOG("%s", tp_or.status().c_message());
    return 1;
  }
  auto tp = std::move(*tp_or);

  auto t_load_or = LoadTraceFile(tp.get(), ctx.platform, global.trace_file);
  if (!t_load_or.ok()) {
    PERFETTO_ELOG("%s", t_load_or.status().c_message());
    return 1;
  }

  // Load spec files.
  std::vector<std::string> spec_content;
  spec_content.reserve(spec_paths.size());
  for (const auto& s : spec_paths) {
    spec_content.emplace_back();
    if (!base::ReadFile(s, &spec_content.back())) {
      PERFETTO_ELOG("Unable to read summary spec file %s", s.c_str());
      return 1;
    }
  }

  std::vector<TraceSummarySpecBytes> specs;
  specs.reserve(spec_paths.size());
  for (uint32_t i = 0; i < spec_paths.size(); ++i) {
    auto format = TraceSummarySpecBytes::Format::kTextProto;
    if (base::EndsWith(spec_paths[i], ".pb")) {
      format = TraceSummarySpecBytes::Format::kBinaryProto;
    }
    specs.emplace_back(TraceSummarySpecBytes{
        reinterpret_cast<const uint8_t*>(spec_content[i].data()),
        spec_content[i].size(),
        format,
    });
  }

  TraceSummaryComputationSpec computation_config;
  if (metrics_v2.empty()) {
    computation_config.v2_metric_ids = std::vector<std::string>();
  } else if (base::CaseInsensitiveEqual(metrics_v2, "all")) {
    computation_config.v2_metric_ids = std::nullopt;
  } else {
    computation_config.v2_metric_ids = base::SplitString(metrics_v2, ",");
  }
  computation_config.metadata_query_id =
      metadata_query.empty() ? std::nullopt
                             : std::make_optional(metadata_query);

  TraceSummaryOutputSpec output_spec;
  if (output_format == "binary") {
    output_spec.format = TraceSummaryOutputSpec::Format::kBinaryProto;
  } else {
    output_spec.format = TraceSummaryOutputSpec::Format::kTextProto;
  }

  std::vector<uint8_t> output;
  auto status = tp->Summarize(computation_config, specs, &output, output_spec);
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }

  if (post_query_path.empty()) {
    fwrite(output.data(), sizeof(char), output.size(), stdout);
  }

  if (!post_query_path.empty()) {
    status = RunQueriesFromFile(tp.get(), post_query_path, true);
    if (!status.ok()) {
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  }

  if (interactive) {
    status = StartInteractiveShell(
        tp.get(),
        InteractiveOptions{20u, MetricV1OutputFormat::kNone, {}, {}, nullptr});
    if (!status.ok()) {
      PERFETTO_ELOG("%s", status.c_message());
      return 1;
    }
  }

  status = MaybeWriteMetatrace(tp.get(), global.metatrace_path);
  if (!status.ok()) {
    PERFETTO_ELOG("%s", status.c_message());
    return 1;
  }
  return 0;
}

}  // namespace perfetto::trace_processor::shell
