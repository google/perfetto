/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/util/symbolizer/symbolize_database.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/progress_reporter.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/util/build_id.h"
#include "src/trace_processor/util/symbolizer/breakpad_symbolizer.h"
#include "src/trace_processor/util/symbolizer/local_symbolizer.h"
#include "src/trace_processor/util/symbolizer/symbolizer.h"

#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::profiling {

namespace {
using trace_processor::Iterator;

// Runtime mapping start/end addresses do not affect symbolization. Group by
// the fields used for address correction and by rel_pc so repeated mappings
// emit one llvm-symbolizer request per effective address.
constexpr const char* kQueryUnsymbolized =
    R"(
      select
        spm.build_id,
        spm.name,
        spm.exact_offset,
        spm.start_offset,
        spm.load_bias,
        spf.rel_pc,
        count(*)
      from __intrinsic_stack_profile_frame spf
      join __intrinsic_stack_profile_mapping spm on spf.mapping = spm.id
      where (
          spm.build_id != ''
          -- The [[] is *not* a typo: that's how you escape [ inside a glob.
          or spm.name GLOB '[[]kernel.kallsyms]*'
        )
        and spf.symbol_set_id IS NULL
      group by
        spm.build_id,
        spm.name,
        spm.exact_offset,
        spm.start_offset,
        spm.load_bias,
        spf.rel_pc
      order by
        spm.build_id,
        spm.name,
        spm.exact_offset,
        spm.start_offset,
        spm.load_bias,
        spf.rel_pc
    )";

// Query to get mappings with empty build IDs and their frame counts.
// These frames cannot be symbolized because we cannot look up symbols without
// a build ID.
constexpr const char* kQueryMappingsWithoutBuildId =
    R"(
      select iif(spm.name = '', '[empty mapping name]', spm.name), count(*)
      from __intrinsic_stack_profile_frame spf
      join __intrinsic_stack_profile_mapping spm on spf.mapping = spm.id
      where spm.build_id = ''
        and spm.name NOT GLOB '[[]kernel.kallsyms]*'
        and spf.symbol_set_id IS NULL
      group by spm.name
    )";

std::vector<std::pair<std::string, uint32_t>> GetMappingsWithoutBuildId(
    trace_processor::TraceProcessor* tp) {
  std::vector<std::pair<std::string, uint32_t>> result;
  Iterator it = tp->ExecuteQuery(kQueryMappingsWithoutBuildId);
  while (it.Next()) {
    std::string name = it.Get(0).AsString();
    int64_t count = it.Get(1).AsLong();
    PERFETTO_CHECK(count >= 0);
    result.emplace_back(std::move(name), static_cast<uint32_t>(count));
  }
  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Failed to query mappings without build ID: %s",
                            it.Status().message().c_str());
  }
  return result;
}

std::optional<std::string> GetOsRelease(trace_processor::TraceProcessor* tp) {
  Iterator it = tp->ExecuteQuery(
      "select str_value from metadata where name = 'system_release'");
  if (it.Next() && it.ColumnCount() > 0 &&
      it.Get(0).type == trace_processor::SqlValue::kString) {
    return it.Get(0).AsString();
  }
  return std::nullopt;
}

// Creates a local symbolizer for "index" mode.
std::unique_ptr<Symbolizer> CreateIndexSymbolizer(
    const SymbolizerConfig& config) {
  if (config.index_symbol_paths.empty() && config.symbol_files.empty()) {
    return nullptr;
  }
  return MaybeLocalSymbolizer(config.index_symbol_paths, config.symbol_files,
                              "index");
}

// Creates a local symbolizer for "find" mode.
std::unique_ptr<Symbolizer> CreateFindSymbolizer(
    const SymbolizerConfig& config) {
  if (config.find_symbol_paths.empty()) {
    return nullptr;
  }
  return MaybeLocalSymbolizer(config.find_symbol_paths, {}, "find");
}

// ModuleSymbols identifies an address by module path, build ID, and rel_pc.
// Keep one selected result per emitted address, including when the trace has
// several mappings with different address-correction parameters.
using MappingKey = std::pair<std::string, std::string>;
struct AddressResult {
  uint32_t frame_count = 0;
  bool binary_found = false;
  bool resolved = false;
};
struct MappingResult {
  std::map<uint64_t, AddressResult> addresses;
  std::map<std::string, uint32_t> successes;
  std::vector<SymbolPathAttempt> attempts;
};
using MappingResults = std::map<MappingKey, MappingResult>;

bool HasFunctionName(const SymbolizedFrame& frame) {
  return !frame.function_name.empty() && frame.function_name != "??";
}

void SymbolizePendingAddresses(const std::vector<UnsymbolizedFrames>& groups,
                               const Symbolizer::Environment& env,
                               Symbolizer* symbolizer,
                               MappingResults* results,
                               std::string* symbols_output) {
  for (const auto& group : groups) {
    auto& result = results->at({group.mapping.name, group.mapping.build_id});
    std::vector<uint64_t> pending;
    for (uint64_t pc : group.rel_pcs) {
      if (!result.addresses.at(pc).resolved)
        pending.push_back(pc);
    }
    if (pending.empty())
      continue;
    auto symbols = symbolizer->Symbolize(env, group.mapping, pending);
    std::string symbol_path;
    for (const auto& attempt : symbols.attempts) {
      if (attempt.error == SymbolPathError::kOk)
        symbol_path = attempt.path;
      auto same_attempt = [&attempt](const SymbolPathAttempt& existing) {
        return existing.path == attempt.path && existing.error == attempt.error;
      };
      if (std::none_of(result.attempts.begin(), result.attempts.end(),
                       same_attempt))
        result.attempts.push_back(attempt);
    }
    if (!symbol_path.empty()) {
      for (uint64_t pc : pending)
        result.addresses.at(pc).binary_found = true;
    }
    if (symbols.frames.empty())
      continue;
    PERFETTO_CHECK(symbols.frames.size() == pending.size());
    protozero::HeapBuffered<protos::pbzero::Trace> trace;
    protos::pbzero::ModuleSymbols* module = nullptr;
    for (size_t i = 0; i < pending.size(); ++i) {
      auto& address = result.addresses.at(pending[i]);
      address.binary_found = true;
      if (!std::any_of(symbols.frames[i].begin(), symbols.frames[i].end(),
                       HasFunctionName))
        continue;
      address.resolved = true;
      result.successes[symbol_path] += address.frame_count;
      if (!module) {
        module = trace->add_packet()->set_module_symbols();
        module->set_path(group.mapping.name);
        module->set_build_id(group.mapping.build_id);
      }
      auto* address_symbols = module->add_address_symbols();
      address_symbols->set_address(pending[i]);
      for (const auto& frame : symbols.frames[i]) {
        auto* line = address_symbols->add_lines();
        line->set_function_name(frame.function_name);
        line->set_source_file_name(frame.file_name);
        line->set_line_number(frame.line);
      }
    }
    if (module)
      *symbols_output += trace.SerializeAsString();
  }
}

void CollectResults(const MappingResults& mappings, SymbolizerResult* result) {
  for (const auto& [key, mapping] : mappings) {
    FailedMapping failure{key.first, key.second, mapping.attempts, 0, 0};
    for (const auto& entry : mapping.addresses) {
      const auto& address = entry.second;
      if (!address.resolved) {
        failure.frame_count += address.frame_count;
        if (address.binary_found)
          failure.frames_without_symbols += address.frame_count;
      }
    }
    for (const auto& [path, count] : mapping.successes)
      result->successful_mappings.push_back(
          {key.first, key.second, path, count});
    if (failure.frame_count)
      result->failed_mappings.push_back(std::move(failure));
  }
}

// ANSI color codes for terminal output.
const char kReset[] = "\x1b[0m";
const char kRed[] = "\x1b[31m";
const char kYellow[] = "\x1b[33m";
const char kCyan[] = "\x1b[36m";

// Helper to wrap text in color codes if colorize is true.
std::string Colorize(bool colorize,
                     const char* color,
                     const std::string& text) {
  if (!colorize) {
    return text;
  }
  return std::string(color) + text + kReset;
}

const char* SymbolPathErrorToString(SymbolPathError error) {
  switch (error) {
    case SymbolPathError::kOk:
      return "ok";
    case SymbolPathError::kFileNotFound:
      return "file not found";
    case SymbolPathError::kBuildIdMismatch:
      return "build ID mismatch";
    case SymbolPathError::kParseError:
      return "failed to parse";
    case SymbolPathError::kBuildIdNotInIndex:
      return "no matching build ID";
  }
  return "unknown";
}

std::string Plural(size_t count, const char* singular, const char* plural) {
  return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

// Formats a hint with optional coloring.
std::string FormatHint(bool colorize, const std::string& text) {
  return Colorize(colorize, kCyan, "hint: " + text);
}

// Hint text for symbol path issues.
std::string SymbolPathHint(bool colorize) {
  return FormatHint(
             colorize,
             "use --symbol-paths to specify symbol files or directories") +
         "\n";
}

// Hint text for missing build IDs.
std::string MissingBuildIdHint(bool colorize) {
  return FormatHint(colorize,
                    "rebuild binaries with build IDs (linker flag "
                    "-Wl,--build-id) and re-record the trace") +
         "\n";
}

// Formats kernel debug symbol installation hint.
void FormatKernelHint(bool colorize, std::string* out, const char* indent) {
  *out += indent;
  *out +=
      FormatHint(colorize, "install kernel debug symbols (vmlinux):") + "\n";
  *out += indent;
  *out +=
      "  Linux (Debian/Ubuntu): sudo apt install "
      "linux-image-$(uname -r)-dbg\n";
  *out += indent;
  *out += "  Linux (Fedora): sudo dnf debuginfo-install kernel\n";
  *out += indent;
  *out += "  Android: obtain vmlinux from your kernel build tree\n";
}

void FormatSuccessfulMappings(const std::vector<SuccessfulMapping>& mappings,
                              std::string* out) {
  uint32_t frame_count = 0;
  for (const auto& mapping : mappings) {
    frame_count += mapping.frame_count;
  }
  if (frame_count == 0) {
    return;
  }
  std::set<MappingKey> unique_mappings;
  for (const auto& mapping : mappings)
    unique_mappings.emplace(mapping.mapping_name, mapping.build_id);
  *out += "\n  Symbolized " + Plural(frame_count, "frame", "frames") +
          " from " + Plural(unique_mappings.size(), "mapping", "mappings") +
          ":\n";
  for (const auto& mapping : mappings) {
    *out += "    " + mapping.mapping_name + " (" +
            Plural(mapping.frame_count, "frame", "frames") + ")";
    *out += " [build ID: " + base::ToHex(mapping.build_id) + "]";
    if (!mapping.symbol_path.empty()) {
      *out += " -> " + mapping.symbol_path;
    }
    *out += "\n";
  }
}

bool IsKernelMapping(const std::string& name) {
  return base::StartsWith(name, "[kernel.kallsyms]");
}

void FormatFailedMappings(bool colorize,
                          const std::vector<FailedMapping>& mappings,
                          std::string* out) {
  uint32_t frame_count = 0;
  for (const auto& mapping : mappings) {
    frame_count += mapping.frame_count;
  }
  if (frame_count == 0) {
    return;
  }
  *out += "\n  Unresolved frames in " +
          Plural(mappings.size(), "mapping", "mappings") + " (" +
          Plural(frame_count, "frame", "frames") + "):\n";
  for (const auto& mapping : mappings) {
    bool is_kernel = IsKernelMapping(mapping.mapping_name);
    *out += "    " + mapping.mapping_name + " (" +
            Plural(mapping.frame_count, "frame", "frames") + ")\n";
    if (!is_kernel) {
      *out += "      build ID: " + base::ToHex(mapping.build_id) + "\n";
    }
    if (mapping.frames_without_symbols) {
      *out += "      " +
              Plural(mapping.frames_without_symbols, "frame", "frames") +
              ": binary found, but no function name for the address\n";
    }
    if (!mapping.attempts.empty()) {
      *out += "      paths searched:\n";
      for (const auto& attempt : mapping.attempts) {
        *out += "        " + attempt.path;
        if (attempt.error != SymbolPathError::kOk) {
          *out +=
              " " +
              Colorize(colorize, kRed,
                       "(" +
                           std::string(SymbolPathErrorToString(attempt.error)) +
                           ")");
        }
        *out += "\n";
      }
    } else {
      *out += "      no paths were configured to search\n";
    }
    if (is_kernel) {
      FormatKernelHint(colorize, out, "      ");
    } else {
      *out += "      ";
      *out += SymbolPathHint(colorize);
    }
  }
}

void FormatSkippedMappings(
    bool colorize,
    const std::vector<std::pair<std::string, uint32_t>>& mappings,
    std::string* out) {
  uint32_t frame_count = 0;
  for (const auto& [name, count] : mappings) {
    frame_count += count;
  }
  if (frame_count == 0) {
    return;
  }
  *out += "\n  No build IDs in trace for " +
          Plural(mappings.size(), "mapping", "mappings") + " (" +
          Plural(frame_count, "frame", "frames") +
          "), symbol lookup requires build IDs:\n";
  for (const auto& [name, count] : mappings) {
    *out += "    " + name + " (" + Plural(count, "frame", "frames") + ")\n";
  }
  *out += "  ";
  *out += MissingBuildIdHint(colorize);
}

}  // namespace

std::vector<UnsymbolizedFrames> CollectUnsymbolizedFrames(
    trace_processor::TraceProcessor* tp) {
  std::vector<UnsymbolizedFrames> result;
  Iterator it = tp->ExecuteQuery(kQueryUnsymbolized);
  while (it.Next()) {
    trace_processor::BuildId build_id =
        trace_processor::BuildId::FromHex(it.Get(0).AsString());
    int64_t load_bias = it.Get(4).AsLong();
    int64_t frame_count = it.Get(6).AsLong();
    PERFETTO_CHECK(load_bias >= 0);
    PERFETTO_CHECK(frame_count >= 0);

    UnsymbolizedMapping mapping{
        build_id.raw(),
        it.Get(1).AsString(),
        static_cast<uint64_t>(it.Get(2).AsLong()),
        static_cast<uint64_t>(it.Get(3).AsLong()),
        static_cast<uint64_t>(load_bias),
    };
    bool same_mapping =
        !result.empty() && result.back().mapping.build_id == mapping.build_id &&
        result.back().mapping.name == mapping.name &&
        result.back().mapping.exact_offset == mapping.exact_offset &&
        result.back().mapping.start_offset == mapping.start_offset &&
        result.back().mapping.load_bias == mapping.load_bias;
    if (!same_mapping)
      result.push_back({std::move(mapping), {}, 0, {}});

    UnsymbolizedFrames& group = result.back();
    PERFETTO_CHECK(static_cast<uint64_t>(group.frame_count) +
                       static_cast<uint64_t>(frame_count) <=
                   std::numeric_limits<uint32_t>::max());
    group.rel_pcs.push_back(static_cast<uint64_t>(it.Get(5).AsLong()));
    group.frame_count += static_cast<uint32_t>(frame_count);
    group.frame_counts.push_back(static_cast<uint32_t>(frame_count));
  }
  if (!it.Status().ok()) {
    PERFETTO_DFATAL_OR_ELOG("Failed to query unsymbolized frames: %s",
                            it.Status().message().c_str());
    return {};
  }
  return result;
}

SymbolizerResult SymbolizeDatabase(trace_processor::TraceProcessor* tp,
                                   const SymbolizerConfig& config) {
  SymbolizerResult result;

  // Get mappings and frame count for frames with empty build IDs.
  result.mappings_without_build_id = GetMappingsWithoutBuildId(tp);

  auto groups = CollectUnsymbolizedFrames(tp);
  MappingResults mappings;
  for (const auto& group : groups) {
    auto& mapping = mappings[{group.mapping.name, group.mapping.build_id}];
    for (size_t i = 0; i < group.rel_pcs.size(); ++i) {
      auto& address = mapping.addresses[group.rel_pcs[i]];
      PERFETTO_CHECK(static_cast<uint64_t>(address.frame_count) +
                         group.frame_counts[i] <=
                     std::numeric_limits<uint32_t>::max());
      address.frame_count += group.frame_counts[i];
    }
  }
  // Nothing needs native symbol lookup (including an already symbolized trace).
  if (groups.empty())
    return result;

  bool has_any_paths =
      !config.index_symbol_paths.empty() || !config.symbol_files.empty() ||
      !config.find_symbol_paths.empty() || !config.breakpad_paths.empty();
  if (!has_any_paths) {
    result.error = SymbolizerError::kSymbolizerNotAvailable;
    result.error_details =
        "no symbol paths were searched. Pass --symbol-paths PATH1,PATH2,... "
        "or enable automatic symbol path discovery.";
    CollectResults(mappings, &result);
    return result;
  }

  Symbolizer::Environment env = {GetOsRelease(tp)};
  if (auto symbolizer = CreateIndexSymbolizer(config); symbolizer)
    SymbolizePendingAddresses(groups, env, symbolizer.get(), &mappings,
                              &result.symbols);
  if (auto symbolizer = CreateFindSymbolizer(config); symbolizer)
    SymbolizePendingAddresses(groups, env, symbolizer.get(), &mappings,
                              &result.symbols);
  for (const auto& path : config.breakpad_paths) {
    BreakpadSymbolizer symbolizer(path);
    SymbolizePendingAddresses(groups, env, &symbolizer, &mappings,
                              &result.symbols);
  }
  CollectResults(mappings, &result);

  result.error = SymbolizerError::kOk;
  return result;
}

std::vector<std::string> GetPerfettoBinaryPath() {
  const char* root = getenv("PERFETTO_BINARY_PATH");
  if (root != nullptr) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    const char* delimiter = ";";
#else
    const char* delimiter = ":";
#endif
    return base::SplitString(root, delimiter);
  }
  return {};
}

std::string FormatSymbolizationSummary(const SymbolizerResult& result,
                                       bool verbose,
                                       bool colorize) {
  std::string summary;

  size_t skipped_count = result.mappings_without_build_id.size();

  // Count total frames.
  uint32_t failed_frames = 0;
  for (const auto& mapping : result.failed_mappings) {
    failed_frames += mapping.frame_count;
  }
  uint32_t skipped_frames = 0;
  for (const auto& [name, count] : result.mappings_without_build_id) {
    skipped_frames += count;
  }
  uint32_t unsymbolized_frames = failed_frames + skipped_frames;

  uint64_t resolved_frames = 0;
  for (const auto& mapping : result.successful_mappings)
    resolved_frames += mapping.frame_count;
  if (resolved_frames + unsymbolized_frames == 0)
    return "No native frames require symbolization.\n";

  summary += Plural(resolved_frames, "frame", "frames") + " symbolized";
  if (unsymbolized_frames) {
    summary += "; " + Colorize(colorize, kYellow,
                               Plural(unsymbolized_frames, "frame", "frames") +
                                   " could not be symbolized");
  }
  summary += ".\n";
  if (!result.error_details.empty())
    summary += result.error_details + "\n";

  if (!verbose) {
    // Non-verbose: show breakdown summary with hints nested under each.
    uint32_t frames_without_symbols = 0;
    size_t missing_binary_mappings = 0;
    for (const auto& mapping : result.failed_mappings) {
      frames_without_symbols += mapping.frames_without_symbols;
      if (mapping.frame_count > mapping.frames_without_symbols)
        ++missing_binary_mappings;
    }
    uint32_t missing_binary_frames = failed_frames - frames_without_symbols;
    if (missing_binary_frames > 0) {
      summary += "  - " + Plural(missing_binary_frames, "frame", "frames") +
                 " from " +
                 Plural(missing_binary_mappings, "mapping", "mappings") +
                 ": no matching symbols in searched paths\n";

      // Add hints nested under "no matching symbols".
      bool has_kernel_failure = false;
      bool has_non_kernel_failure = false;
      for (const auto& mapping : result.failed_mappings) {
        if (IsKernelMapping(mapping.mapping_name)) {
          has_kernel_failure = true;
        } else {
          has_non_kernel_failure = true;
        }
      }
      if (has_non_kernel_failure) {
        summary += "    ";
        summary += SymbolPathHint(colorize);
      }
      if (has_kernel_failure) {
        FormatKernelHint(colorize, &summary, "    ");
      }
    }
    if (frames_without_symbols) {
      summary += "  - " + Plural(frames_without_symbols, "frame", "frames") +
                 ": binary found, but no function name for the address\n";
    }
    if (skipped_frames > 0) {
      summary += "  - " + Plural(skipped_frames, "frame", "frames") + " from " +
                 Plural(skipped_count, "mapping", "mappings") +
                 ": no build IDs in trace, symbol lookup requires build IDs\n";
      summary += "    ";
      summary += MissingBuildIdHint(colorize);
    }

    if (unsymbolized_frames)
      summary += "Use --verbose to see the full details.\n";
    return summary;
  }

  // Verbose output - show everything.
  FormatSuccessfulMappings(result.successful_mappings, &summary);
  FormatFailedMappings(colorize, result.failed_mappings, &summary);
  FormatSkippedMappings(colorize, result.mappings_without_build_id, &summary);

  return summary;
}

SymbolizerResult SymbolizeDatabaseAndLog(trace_processor::TraceProcessor* tp,
                                         const SymbolizerConfig& config,
                                         bool verbose,
                                         bool quiet) {
  SymbolizerResult result = SymbolizeDatabase(tp, config);
  if (!quiet || !result.failed_mappings.empty() ||
      !result.mappings_without_build_id.empty()) {
    std::string summary = FormatSymbolizationSummary(
        result, verbose && !quiet, base::StderrSupportsColor());
    fprintf(stderr, "%s", summary.c_str());
  }
  return result;
}

}  // namespace perfetto::profiling
