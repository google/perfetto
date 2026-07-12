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

#include "src/trace_processor/shell/report_subcommand.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/shell/common_flags.h"
#include "src/trace_processor/shell/metatrace.h"
#include "src/trace_processor/shell/report/report_sink.h"
#include "src/trace_processor/shell/report/report_view.h"
#include "src/trace_processor/shell/report/scope.h"
#include "src/trace_processor/shell/report/slices_view.h"
#include "src/trace_processor/shell/report/text_renderer.h"
#include "src/trace_processor/shell/subcommand.h"
#include "src/trace_processor/util/simple_json_serializer.h"

namespace perfetto::trace_processor::shell {
namespace {

template <typename T>
std::unique_ptr<ReportView> MakeView() {
  return std::make_unique<T>();
}

struct ViewEntry {
  const char* view;
  std::unique_ptr<ReportView> (*make)();
};

struct NounSpec {
  const char* noun;
  const char* default_view;
  std::vector<ViewEntry> views;  // empty for the overview, which fans out
};

// The closed set of nouns and the views implementing them. Registering a view
// here is all that is needed: running, --format sql and the overview fan-out
// pick it up automatically.
const std::vector<NounSpec>& Nouns() {
  static const auto* nouns = new std::vector<NounSpec>{
      {"overview", "", {}},
      {"slices", "flat", {{"flat", &MakeView<SlicesTableView>}}},
  };
  return *nouns;
}

const NounSpec* FindNoun(const std::string& noun) {
  for (const auto& n : Nouns()) {
    if (noun == n.noun)
      return &n;
  }
  return nullptr;
}

std::string NounList() {
  std::string out;
  for (const auto& n : Nouns()) {
    if (!out.empty())
      out += ", ";
    out += n.noun;
  }
  return out;
}

std::string ViewList(const NounSpec& spec) {
  std::string out;
  for (const auto& v : spec.views) {
    if (!out.empty())
      out += ", ";
    out += v.view;
  }
  return out;
}

bool IsKnownView(const NounSpec& spec, const std::string& view) {
  for (const auto& v : spec.views) {
    if (view == v.view)
      return true;
  }
  return false;
}

// The views to run for a resolved noun/view. For the overview this is every
// other noun's default view; otherwise the single matching view.
std::vector<std::unique_ptr<ReportView>> ViewsFor(const std::string& noun,
                                                  const std::string& view) {
  std::vector<std::unique_ptr<ReportView>> out;
  if (noun == "overview") {
    for (const auto& n : Nouns()) {
      if (std::string(n.noun) == "overview")
        continue;
      for (const auto& e : n.views) {
        if (std::string(e.view) == n.default_view)
          out.push_back(e.make());
      }
    }
    return out;
  }
  const NounSpec* spec = FindNoun(noun);
  for (const auto& e : spec->views) {
    if (view == e.view)
      out.push_back(e.make());
  }
  return out;
}

}  // namespace

base::StatusOr<ParsedReportArgs> ParseReportArgs(
    const std::vector<std::string>& positional,
    bool remote) {
  // Grammar: report [noun [view]] <trace_file>. The noun and view are parsed
  // from the front so a leading known noun (e.g. `report slices`) is always
  // treated as the noun, never as the trace file. The trace file (local mode
  // only) is whatever remains after the noun/view.
  ParsedReportArgs args;
  args.noun = "overview";

  // In local mode one trailing positional is reserved for the trace file.
  const size_t trace_slots = remote ? 0 : 1;

  size_t i = 0;
  if (!positional.empty() && FindNoun(positional[0])) {
    args.noun = positional[i++];
  } else if (positional.size() > trace_slots) {
    // The leading token sits in the noun slot (a trace file still follows it)
    // but is not a known noun.
    return base::ErrStatus("report: unknown noun '%s'; valid nouns: %s",
                           positional[0].c_str(), NounList().c_str());
  }
  const NounSpec* spec = FindNoun(args.noun);
  size_t remaining = positional.size() - i;

  // A view is present only if there is a token beyond the trace-file slot.
  args.view = spec->default_view;
  if (remaining > trace_slots) {
    const std::string& view = positional[i];
    if (!IsKnownView(*spec, view)) {
      return base::ErrStatus(
          "report: unknown view '%s' for '%s'; valid views: %s", view.c_str(),
          args.noun.c_str(), ViewList(*spec).c_str());
    }
    args.view = view;
    ++i;
    remaining = positional.size() - i;
  }

  if (!remote) {
    if (remaining == 0)
      return base::ErrStatus("report: trace file is required");
    args.trace_file = positional[i++];
    remaining = positional.size() - i;
  }
  if (remaining > 0) {
    return base::ErrStatus("report: unexpected argument '%s'",
                           positional[i].c_str());
  }
  return args;
}

const char* ReportSubcommand::name() const {
  return "report";
}

const char* ReportSubcommand::description() const {
  return "Print opinionated, built-in summaries of a trace.";
}

const char* ReportSubcommand::usage_args() const {
  return "[noun [view]] <trace_file>";
}

const char* ReportSubcommand::detailed_help() const {
  return R"(Print zero-config summaries over common trace dimensions.

With no noun, prints an overview of what is in the trace. Otherwise pass a
noun (and optionally a view) to drill in:
  report slices <trace>            slices aggregated by name

Output is a stream of self-delimited packets. --format selects the encoding:
text (default, human tables), jsonl (one packet per line), binary
(length-delimited proto, truncatable), or sql (print the underlying queries
instead of running them).)";
}

std::vector<FlagSpec> ReportSubcommand::GetFlags() {
  return {
      StringFlag("format", '\0', "FORMAT",
                 "Output: text (default), jsonl, binary, sql.", &format_),
      StringFlag("top", '\0', "N", "Max rows per view (default 10).", &top_),
      StringFlag("name", '\0', "GLOB", "Filter rows by name (GLOB).",
                 &name_glob_),
  };
}

base::Status ReportSubcommand::Run(const SubcommandContext& ctx) {
  base::Status status = RunInner(ctx);
  if (status.ok())
    return status;
  // Under a JSON encoding, surface failures as a structured error object on
  // stdout so machine consumers get one shape for success and failure.
  if (base::CaseInsensitiveEqual(format_, "json") ||
      base::CaseInsensitiveEqual(format_, "jsonl")) {
    std::string out = json::SerializeJson([&](json::JsonValueSerializer&& w) {
      std::move(w).WriteDict([&](json::JsonDictSerializer& d) {
        d.AddDict("error", [&](json::JsonDictSerializer& e) {
          e.AddString("message", status.message());
        });
      });
    });
    printf("%s\n", out.c_str());
    return base::OkStatus();
  }
  return status;
}

base::Status ReportSubcommand::RunInner(const SubcommandContext& ctx) {
  bool remote = !ctx.global->remote_addr.empty();
  ASSIGN_OR_RETURN(ParsedReportArgs args,
                   ParseReportArgs(ctx.positional_args, remote));
  const std::string& noun = args.noun;
  const std::string& trace_file = args.trace_file;

  Scope scope;
  if (!top_.empty()) {
    auto parsed = base::StringToInt64(top_);
    if (!parsed || *parsed <= 0)
      return base::ErrStatus("report: --top must be a positive integer");
    scope.top = *parsed;
  }
  scope.name_glob = name_glob_;

  std::vector<std::unique_ptr<ReportView>> views = ViewsFor(noun, args.view);
  const std::string& format = format_.empty() ? std::string("text") : format_;

  // `sql` is a dry run: print each view's query without loading the trace.
  if (base::CaseInsensitiveEqual(format, "sql")) {
    for (const auto& v : views)
      printf("%s;\n", v->Sql(scope).c_str());
    return base::OkStatus();
  }

  base::TimeNanos t_load{};
  ASSIGN_OR_RETURN(auto tp, CreateTraceProcessor(*ctx.global, ctx.platform,
                                                 trace_file, &t_load));

  bool overview = noun == "overview";
  std::unique_ptr<ReportSink> sink;
  if (base::CaseInsensitiveEqual(format, "text")) {
    sink = std::make_unique<TextSink>(stdout, overview);
  } else if (base::CaseInsensitiveEqual(format, "binary")) {
    sink = std::make_unique<BinarySink>(stdout);
  } else if (base::CaseInsensitiveEqual(format, "jsonl") ||
             base::CaseInsensitiveEqual(format, "json")) {
    ASSIGN_OR_RETURN(auto jsonl, JsonlSink::Create(stdout));
    sink = std::move(jsonl);
  } else {
    return base::ErrStatus(
        "report: unknown --format '%s'; valid formats: text, jsonl, binary, "
        "sql",
        format.c_str());
  }

  RETURN_IF_ERROR(EmitHeader(tp.get(), trace_file, sink.get()));
  // Skip empty sections only in the overview fan-out.
  for (const auto& v : views)
    RETURN_IF_ERROR(v->Emit(tp.get(), scope, sink.get(), overview));
  RETURN_IF_ERROR(sink->Finalize());

  RETURN_IF_ERROR(MaybeWriteMetatrace(tp.get(), ctx.global->metatrace_path));
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
