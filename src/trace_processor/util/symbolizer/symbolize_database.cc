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
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/murmur_hash.h"
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
      select spm.name, count(*)
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
struct MappingKey {
  std::string name;
  std::string build_id;
  bool operator==(const MappingKey& other) const {
    return name == other.name && build_id == other.build_id;
  }
};
struct MappingKeyHasher {
  size_t operator()(const MappingKey& key) const {
    return static_cast<size_t>(base::MurmurHashCombine(key.name, key.build_id));
  }
};
struct AddressResult {
  uint32_t frame_count = 0;
  bool binary_found = false;
  bool resolved = false;
};
struct MappingResult {
  MappingKey key;
  base::FlatHashMapV2<uint64_t, AddressResult> addresses;
  // Resolved frame count per symbol path, in the order paths were used.
  std::vector<std::pair<std::string, uint32_t>> successes;
  std::vector<SymbolPathAttempt> attempts;
};
struct MappingResults {
  // In first-seen order, so that reports are stable across runs.
  std::vector<MappingResult> mappings;
  // Parallel to the groups passed to SymbolizePendingAddresses().
  std::vector<size_t> group_to_mapping;
};

bool HasFunctionName(const SymbolizedFrame& frame) {
  return !frame.function_name.empty() && frame.function_name != "??";
}

void SymbolizePendingAddresses(const std::vector<UnsymbolizedFrames>& groups,
                               const Symbolizer::Environment& env,
                               Symbolizer* symbolizer,
                               MappingResults* results,
                               std::string* symbols_output) {
  for (size_t g = 0; g < groups.size(); ++g) {
    const auto& group = groups[g];
    auto& result = results->mappings[results->group_to_mapping[g]];
    std::vector<uint64_t> pending;
    for (uint64_t pc : group.rel_pcs) {
      if (!result.addresses.Find(pc)->resolved)
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
        result.addresses.Find(pc)->binary_found = true;
    }
    if (symbols.frames.empty())
      continue;
    PERFETTO_CHECK(symbols.frames.size() == pending.size());
    protozero::HeapBuffered<protos::pbzero::Trace> trace;
    protos::pbzero::ModuleSymbols* module = nullptr;
    for (size_t i = 0; i < pending.size(); ++i) {
      auto& address = *result.addresses.Find(pending[i]);
      address.binary_found = true;
      if (!std::any_of(symbols.frames[i].begin(), symbols.frames[i].end(),
                       HasFunctionName))
        continue;
      address.resolved = true;
      auto success = std::find_if(
          result.successes.begin(), result.successes.end(),
          [&symbol_path](const auto& s) { return s.first == symbol_path; });
      if (success == result.successes.end())
        success = result.successes.emplace(success, symbol_path, 0);
      success->second += address.frame_count;
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

void CollectResults(const MappingResults& results, SymbolizerResult* result) {
  for (const auto& mapping : results.mappings) {
    const auto& key = mapping.key;
    FailedMapping failure{key.name, key.build_id, mapping.attempts, 0, 0};
    for (auto it = mapping.addresses.GetIterator(); it; ++it) {
      const auto& address = it.value();
      if (!address.resolved) {
        failure.frame_count += address.frame_count;
        if (address.binary_found)
          failure.frames_without_symbols += address.frame_count;
      }
    }
    for (const auto& [path, count] : mapping.successes)
      result->successful_mappings.push_back(
          {key.name, key.build_id, path, count, mapping.attempts});
    if (failure.frame_count)
      result->failed_mappings.push_back(std::move(failure));
  }
}

// ANSI color codes for terminal output.
// One role per color: bold for the section label, green for what worked,
// yellow for counts of what did not, cyan for advice, gray for supporting
// detail and red only for a file that exists but cannot be used.
const char kReset[] = "\x1b[0m";
const char kBold[] = "\x1b[1m";
const char kRed[] = "\x1b[31m";
const char kGreen[] = "\x1b[32m";
const char kYellow[] = "\x1b[33m";
const char kBoldCyan[] = "\x1b[1;36m";
const char kGray[] = "\x1b[90m";

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
      return "found";
    case SymbolPathError::kFileNotFound:
      return "file not found";
    case SymbolPathError::kBuildIdMismatch:
      return "build ID mismatch";
    case SymbolPathError::kParseError:
      return "failed to parse";
    case SymbolPathError::kBuildIdNotInIndex:
      return "no matching build ID";
    case SymbolPathError::kNotOnServer:
      return "not on server";
    case SymbolPathError::kServerUnreachable:
      return "server unreachable";
    case SymbolPathError::kDownloadFailed:
      return "download failed";
  }
  return "unknown";
}

std::string Plural(size_t count, const char* singular, const char* plural) {
  return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

std::string Frames(size_t count) {
  return Plural(count, "frame", "frames");
}

std::string Mappings(size_t count) {
  return Plural(count, "mapping", "mappings");
}

std::string LlvmSymbolizerUnavailableMessage() {
  return "llvm-symbolizer could not be run, so no symbols were read from "
         "native binaries. Install LLVM (e.g. 'apt install llvm' or "
         "'brew install llvm') and make sure llvm-symbolizer is on the PATH.\n";
}

bool IsKernelMapping(const std::string& name) {
  return base::StartsWith(name, "[kernel.kallsyms]");
}

bool IsAndroidPlatformMapping(const std::string& name) {
  for (const char* prefix : {"/apex/", "/system/", "/system_ext/", "/vendor/",
                             "/product/", "/odm/"}) {
    if (base::StartsWith(name, prefix))
      return true;
  }
  return false;
}

bool IsLinuxDistroMapping(const std::string& name) {
  for (const char* prefix :
       {"/usr/bin/", "/usr/sbin/", "/usr/lib/", "/usr/lib64/", "/usr/libexec/",
        "/bin/", "/sbin/", "/lib/", "/lib64/"}) {
    if (base::StartsWith(name, prefix))
      return true;
  }
  return false;
}

// Why frames stayed unresolved, in the order the report lists them.
enum class UnresolvedReason {
  kNotSearched,
  kBinaryNotFound,
  kNoSymbolForAddress,
  kKernel,
  kNoBuildId,
  kAnonymous,
  kMax,
};

struct UnresolvedMapping {
  std::string name;
  std::string build_id;
  uint32_t frame_count = 0;
  // Null when no symbol source was asked about this mapping.
  const std::vector<SymbolPathAttempt>* attempts = nullptr;
};

struct UnresolvedGroup {
  UnresolvedReason reason = UnresolvedReason::kMax;
  uint32_t frame_count = 0;
  std::vector<UnresolvedMapping> mappings;
};

std::string ReasonText(UnresolvedReason reason, bool llvm_unavailable) {
  switch (reason) {
    case UnresolvedReason::kNotSearched:
      return llvm_unavailable ? "llvm-symbolizer could not be run"
                              : "no symbol paths were searched";
    case UnresolvedReason::kBinaryNotFound:
      return "no binary with a matching build ID in the searched paths";
    case UnresolvedReason::kNoSymbolForAddress:
      return "binary found, but it has no symbol for the address";
    case UnresolvedReason::kKernel:
      return "kernel frames, no vmlinux in the searched paths";
    case UnresolvedReason::kNoBuildId:
      return "no build ID recorded, so symbols cannot be looked up";
    case UnresolvedReason::kAnonymous:
      return "no backing file (JIT, anonymous or [vdso]-style mappings)";
    case UnresolvedReason::kMax:
      break;
  }
  return "";
}

// Introduces the list of paths searched in the verbose report.
std::string ReasonWithPathsText(UnresolvedReason reason) {
  switch (reason) {
    case UnresolvedReason::kBinaryNotFound:
      return "no binary with a matching build ID in:";
    case UnresolvedReason::kNoSymbolForAddress:
      return "binary found, but it has no symbol for the address; searched:";
    case UnresolvedReason::kKernel:
      return "no kernel symbols (vmlinux) in:";
    case UnresolvedReason::kNotSearched:
    case UnresolvedReason::kNoBuildId:
    case UnresolvedReason::kAnonymous:
    case UnresolvedReason::kMax:
      break;
  }
  return "";
}

std::vector<UnresolvedGroup> GroupUnresolved(const SymbolizerResult& result) {
  std::vector<UnresolvedGroup> groups(
      static_cast<size_t>(UnresolvedReason::kMax));
  for (size_t i = 0; i < groups.size(); ++i)
    groups[i].reason = static_cast<UnresolvedReason>(i);
  auto add = [&groups](UnresolvedReason reason, UnresolvedMapping mapping) {
    auto& group = groups[static_cast<size_t>(reason)];
    group.frame_count += mapping.frame_count;
    group.mappings.push_back(std::move(mapping));
  };
  for (const auto& mapping : result.failed_mappings) {
    UnresolvedMapping entry{mapping.mapping_name, mapping.build_id,
                            mapping.frame_count, &mapping.attempts};
    if (IsKernelMapping(mapping.mapping_name)) {
      add(UnresolvedReason::kKernel, std::move(entry));
    } else if (mapping.attempts.empty()) {
      add(UnresolvedReason::kNotSearched, std::move(entry));
    } else {
      uint32_t missing = mapping.frame_count - mapping.frames_without_symbols;
      if (missing) {
        entry.frame_count = missing;
        add(UnresolvedReason::kBinaryNotFound, entry);
      }
      if (mapping.frames_without_symbols) {
        entry.frame_count = mapping.frames_without_symbols;
        add(UnresolvedReason::kNoSymbolForAddress, std::move(entry));
      }
    }
  }
  for (const auto& [name, count] : result.mappings_without_build_id) {
    UnresolvedMapping entry{name, {}, count, nullptr};
    bool no_file = name.empty() || name[0] == '[';
    add(no_file ? UnresolvedReason::kAnonymous : UnresolvedReason::kNoBuildId,
        std::move(entry));
  }
  // Largest first, so the short report names the mappings that matter most.
  for (auto& group : groups) {
    std::stable_sort(
        group.mappings.begin(), group.mappings.end(),
        [](const UnresolvedMapping& a, const UnresolvedMapping& b) {
          return a.frame_count > b.frame_count;
        });
  }
  return groups;
}

std::string DisplayName(const std::string& name) {
  return name.empty() ? "[anonymous]" : name;
}

bool AnyMapping(const std::vector<UnresolvedGroup>& groups,
                std::initializer_list<UnresolvedReason> reasons,
                bool (*pred)(const std::string&)) {
  for (auto reason : reasons) {
    const auto& group = groups[static_cast<size_t>(reason)];
    if (std::any_of(
            group.mappings.begin(), group.mappings.end(),
            [pred](const UnresolvedMapping& m) { return pred(m.name); })) {
      return true;
    }
  }
  return false;
}

void FormatEnableDiscoveryAdvice(bool colorize, std::string* out) {
  *out += "    " + Colorize(colorize, kBold, "Otherwise,") +
          " enable automatic symbol path discovery to search the usual "
          "locations.\n";
}

// Advice for frames whose binaries were not searched for, or were searched
// for but not usable, split by where the reader gets their binaries from
// rather than by platform.
void FormatFixAdvice(bool colorize,
                     bool llvm_unavailable,
                     bool debuginfod_enabled,
                     const std::vector<UnresolvedGroup>& groups,
                     std::string* out) {
  auto frames = [&groups](UnresolvedReason reason) {
    return groups[static_cast<size_t>(reason)].frame_count;
  };
  // With llvm-symbolizer missing the warning above already names the fix.
  bool not_searched =
      frames(UnresolvedReason::kNotSearched) > 0 && !llvm_unavailable;
  bool missing = frames(UnresolvedReason::kBinaryNotFound) ||
                 frames(UnresolvedReason::kNoSymbolForAddress);
  bool kernel = frames(UnresolvedReason::kKernel) > 0;
  if (!not_searched && !missing && !kernel)
    return;
  const auto kNative = {UnresolvedReason::kNotSearched,
                        UnresolvedReason::kBinaryNotFound,
                        UnresolvedReason::kNoSymbolForAddress};
  bool linux_distro = AnyMapping(groups, kNative, IsLinuxDistroMapping);
  bool android = AnyMapping(groups, kNative, IsAndroidPlatformMapping);

  *out += "  " + Colorize(colorize, kBoldCyan, "To fix this:") + "\n";
  *out += "    " +
          Colorize(colorize, kBold, "If you build these binaries yourself,") +
          " pass --symbol-paths DIR1,DIR2,... pointing at the unstripped "
          "build outputs";
  *out += kernel ? " or vmlinux.\n" : ".\n";
  *out += "    " + Colorize(colorize, kBold, "If they come from your OS,") +
          " install its debug symbols";
  if (!linux_distro && !kernel && !android) {
    *out += "; see https://perfetto.dev/docs/learning-more/symbolization\n";
    if (not_searched)
      FormatEnableDiscoveryAdvice(colorize, out);
    return;
  }
  *out += ":\n";
  if (linux_distro || kernel) {
    *out += "      " + Colorize(colorize, kBold, "Debian/Ubuntu:") +
            " sudo apt install <package>-dbgsym";
    if (kernel)
      *out += ", kernel: linux-image-$(uname -r)-dbg";
    *out += "\n      " + Colorize(colorize, kBold, "Fedora:") +
            " sudo dnf debuginfo-install <package>";
    if (kernel)
      *out += ", kernel: sudo dnf debuginfo-install kernel";
    *out += "\n";
    if (!debuginfod_enabled) {
      *out += "      " + Colorize(colorize, kBold, "Any distro:") +
              " pass --debuginfod to fetch them from its debuginfod server "
              "(DEBUGINFOD_URLS)\n";
    }
  }
  if (android) {
    *out += "      " + Colorize(colorize, kBold, "Android:") +
            " use the symbols directory of the matching platform build "
            "(ANDROID_PRODUCT_OUT/symbols)\n";
  }
  if (not_searched)
    FormatEnableDiscoveryAdvice(colorize, out);
}

// A file that exists but is unusable, or a server that did not answer, is the
// one worth noticing.
std::string FormatAttemptOutcome(bool colorize,
                                 const SymbolPathAttempt& attempt) {
  const char* color = kGray;
  if (attempt.error == SymbolPathError::kOk)
    color = kGreen;
  else if (attempt.error == SymbolPathError::kBuildIdMismatch ||
           attempt.error == SymbolPathError::kParseError ||
           attempt.error == SymbolPathError::kServerUnreachable ||
           attempt.error == SymbolPathError::kDownloadFailed)
    color = kRed;
  std::string outcome = SymbolPathErrorToString(attempt.error);
  if (!attempt.detail.empty())
    outcome += ": " + attempt.detail;
  return Colorize(colorize, color, outcome);
}

void FormatSuccessfulMappings(bool colorize,
                              const std::vector<SuccessfulMapping>& mappings,
                              std::string* out) {
  uint32_t frame_count = 0;
  for (const auto& mapping : mappings)
    frame_count += mapping.frame_count;
  if (frame_count == 0)
    return;
  // Entries for one mapping are contiguous, one per symbol path used.
  auto same_mapping = [&mappings](size_t i) {
    return i > 0 && mappings[i].mapping_name == mappings[i - 1].mapping_name &&
           mappings[i].build_id == mappings[i - 1].build_id;
  };
  size_t unique_mappings = 0;
  for (size_t i = 0; i < mappings.size(); ++i)
    unique_mappings += same_mapping(i) ? 0 : 1;
  *out += "\n  " +
          Colorize(colorize, kGreen,
                   "Symbolized " + Frames(frame_count) + " from " +
                       Mappings(unique_mappings) + ":") +
          "\n";
  for (size_t i = 0; i < mappings.size(); ++i) {
    const auto& mapping = mappings[i];
    if (!same_mapping(i)) {
      uint32_t mapping_frames = 0;
      for (size_t j = i; j < mappings.size() && (j == i || same_mapping(j));
           ++j)
        mapping_frames += mappings[j].frame_count;
      *out += "    " + DisplayName(mapping.mapping_name) + " (" +
              Frames(mapping_frames) + ")\n";
      *out += "      " +
              Colorize(colorize, kGray,
                       "build ID: " + base::ToHex(mapping.build_id)) +
              "\n";
    }
    if (!mapping.symbol_path.empty()) {
      std::string source = Frames(mapping.frame_count);
      for (const auto& attempt : mapping.attempts) {
        if (attempt.error == SymbolPathError::kOk &&
            attempt.path == mapping.symbol_path && !attempt.detail.empty())
          source += ", " + attempt.detail;
      }
      *out +=
          "      symbols: " + Colorize(colorize, kGreen, mapping.symbol_path) +
          " (" + source + ")\n";
    }
    // Sources that did not help, listed once after the mapping's last entry.
    bool last = i + 1 == mappings.size() || !same_mapping(i + 1);
    if (!last)
      continue;
    bool any_failed = false;
    for (const auto& attempt : mapping.attempts)
      any_failed |= attempt.error != SymbolPathError::kOk;
    if (!any_failed)
      continue;
    *out += "      also tried:\n";
    for (const auto& attempt : mapping.attempts) {
      if (attempt.error == SymbolPathError::kOk)
        continue;
      *out += "        " + Colorize(colorize, kGray, attempt.path) + " (" +
              FormatAttemptOutcome(colorize, attempt) + ")\n";
    }
  }
}

void FormatUnresolvedMappings(bool colorize,
                              bool llvm_unavailable,
                              const std::vector<UnresolvedGroup>& groups,
                              std::string* out) {
  uint32_t frame_count = 0;
  size_t mapping_count = 0;
  for (const auto& group : groups) {
    frame_count += group.frame_count;
    mapping_count += group.mappings.size();
  }
  if (frame_count == 0)
    return;
  *out += "\n  " +
          Colorize(colorize, kYellow,
                   "Could not symbolize " + Frames(frame_count) + " from " +
                       Mappings(mapping_count) + ":") +
          "\n";
  for (const auto& group : groups) {
    for (const auto& mapping : group.mappings) {
      *out += "    " + DisplayName(mapping.name) + " (" +
              Frames(mapping.frame_count) + ")\n";
      if (!mapping.build_id.empty()) {
        *out += "      " +
                Colorize(colorize, kGray,
                         "build ID: " + base::ToHex(mapping.build_id)) +
                "\n";
      }
      if (!mapping.attempts || mapping.attempts->empty()) {
        *out += "      " + ReasonText(group.reason, llvm_unavailable) + "\n";
        continue;
      }
      *out += "      " + ReasonWithPathsText(group.reason) + "\n";
      for (const auto& attempt : *mapping.attempts) {
        *out += "        " + Colorize(colorize, kGray, attempt.path) + " (" +
                FormatAttemptOutcome(colorize, attempt) + ")\n";
      }
    }
  }
}

// The short report names the affected mappings inline, but keeps long lists
// for --verbose.
constexpr size_t kMaxInlineMappings = 3;

bool IsServerUrl(const std::string& path) {
  return base::StartsWith(path, "http://") ||
         base::StartsWith(path, "https://");
}

// Download errors and bad files from servers are worth a line in the short
// report, since a negative answer from a reachable server is not. Each
// distinct cause is named once.
void FormatDebuginfodFailureHints(bool colorize,
                                  const std::vector<UnresolvedGroup>& groups,
                                  std::string* out) {
  std::vector<std::string> causes;
  for (auto reason : {UnresolvedReason::kBinaryNotFound,
                      UnresolvedReason::kNoSymbolForAddress}) {
    for (const auto& mapping : groups[static_cast<size_t>(reason)].mappings) {
      if (!mapping.attempts)
        continue;
      for (const auto& attempt : *mapping.attempts) {
        bool download_error =
            attempt.error == SymbolPathError::kDownloadFailed ||
            (IsServerUrl(attempt.path) &&
             (attempt.error == SymbolPathError::kParseError ||
              attempt.error == SymbolPathError::kBuildIdMismatch));
        if (!download_error)
          continue;
        std::string cause = attempt.detail.empty()
                                ? SymbolPathErrorToString(attempt.error)
                                : attempt.detail;
        cause += " (" + attempt.path + ")";
        if (std::find(causes.begin(), causes.end(), cause) == causes.end())
          causes.push_back(std::move(cause));
      }
    }
  }
  for (const auto& cause : causes) {
    *out += "    " + Colorize(colorize, kBoldCyan, "hint:") +
            " debuginfod: " + cause + "\n";
  }
}

void FormatUnresolvedSummary(bool colorize,
                             bool llvm_unavailable,
                             const DebuginfodStats& debuginfod,
                             const std::vector<UnresolvedGroup>& groups,
                             std::string* out) {
  for (const auto& group : groups) {
    // Not found and found-but-stripped need the same fix, so the short report
    // presents them as one group.
    if (group.reason == UnresolvedReason::kNoSymbolForAddress)
      continue;
    uint32_t frame_count = group.frame_count;
    // Unnamed mappings count but are not worth listing.
    size_t mapping_count = 0;
    std::vector<std::string> names;
    std::string reason = ReasonText(group.reason, llvm_unavailable);
    auto collect = [&names, &mapping_count](const UnresolvedGroup& g) {
      for (const auto& mapping : g.mappings) {
        if (mapping.name.empty()) {
          ++mapping_count;
        } else if (std::find(names.begin(), names.end(), mapping.name) ==
                   names.end()) {
          names.push_back(mapping.name);
        }
      }
    };
    collect(group);
    if (group.reason == UnresolvedReason::kBinaryNotFound) {
      const auto& stripped =
          groups[static_cast<size_t>(UnresolvedReason::kNoSymbolForAddress)];
      frame_count += stripped.frame_count;
      collect(stripped);
      reason = debuginfod.enabled
                   ? "no usable symbols in the searched paths or servers"
                   : "no usable symbols in the searched paths";
    }
    if (frame_count == 0)
      continue;
    *out += "  " + Colorize(colorize, kYellow, Frames(frame_count)) + " from " +
            Mappings(mapping_count + names.size()) + ": " + reason + "\n";
    size_t shown = std::min(names.size(), kMaxInlineMappings);
    for (size_t i = 0; i < shown; ++i)
      *out += "    " + names[i] + "\n";
    if (shown < names.size()) {
      *out += "    " +
              Colorize(
                  colorize, kGray,
                  "... and " + std::to_string(names.size() - shown) + " more") +
              "\n";
    }
    if (group.reason == UnresolvedReason::kNotSearched && !llvm_unavailable) {
      *out += "    " + Colorize(colorize, kBoldCyan, "hint:") +
              " pass --symbol-paths DIR1,DIR2,... or enable automatic symbol "
              "path discovery\n";
    } else if (group.reason == UnresolvedReason::kNoBuildId) {
      *out += "    " + Colorize(colorize, kBoldCyan, "hint:") +
              " rebuild with build IDs (linker flag -Wl,--build-id) and "
              "re-record the trace\n";
    } else if (group.reason == UnresolvedReason::kBinaryNotFound) {
      for (const auto& server : debuginfod.unreachable_servers) {
        *out += "    " + Colorize(colorize, kBoldCyan, "hint:") +
                " debuginfod server " + server +
                " could not be reached; check the URL and your network\n";
      }
      FormatDebuginfodFailureHints(colorize, groups, out);
    }
  }
}

// One line of counts for the run, plus any server that could not be reached.
// Warnings about missing tools are printed even when nothing was looked up.
void FormatDebuginfod(bool colorize,
                      const DebuginfodStats& stats,
                      std::string* out) {
  if (!stats.warnings.empty())
    *out += Colorize(colorize, kYellow, stats.warnings);
  std::vector<std::string> parts;
  if (stats.downloads)
    parts.push_back(std::to_string(stats.downloads) + " downloaded");
  if (stats.cache_hits)
    parts.push_back(std::to_string(stats.cache_hits) + " from cache");
  if (stats.not_found)
    parts.push_back(std::to_string(stats.not_found) +
                    " not found on any server");
  if (stats.failed)
    parts.push_back(std::to_string(stats.failed) + " failed");
  if (parts.empty())
    return;
  *out += "  " +
          Colorize(colorize, kGray, "Debuginfod: " + base::Join(parts, ", ")) +
          "\n";
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
  base::FlatHashMapV2<MappingKey, size_t, MappingKeyHasher> mapping_index;
  for (const auto& group : groups) {
    MappingKey key{group.mapping.name, group.mapping.build_id};
    auto [idx, inserted] = mapping_index.Insert(key, mappings.mappings.size());
    if (inserted)
      mappings.mappings.emplace_back().key = key;
    mappings.group_to_mapping.push_back(*idx);
    auto& mapping = mappings.mappings[*idx];
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
      !config.find_symbol_paths.empty() || !config.breakpad_paths.empty() ||
      !config.debuginfod.urls.empty();
  if (!has_any_paths) {
    result.error = SymbolizerError::kSymbolizerNotAvailable;
    result.error_details =
        "no symbol paths were searched. Pass --symbol-paths PATH1,PATH2,... "
        "or enable automatic symbol path discovery.";
    CollectResults(mappings, &result);
    return result;
  }

  // Only require llvm-symbolizer if a local symbol source is configured.
  bool has_local_paths = !config.index_symbol_paths.empty() ||
                         !config.symbol_files.empty() ||
                         !config.find_symbol_paths.empty();
  if (has_local_paths && !IsLlvmSymbolizerAvailable())
    result.llvm_symbolizer_unavailable = true;

  Symbolizer::Environment env = {GetOsRelease(tp)};
  if (!result.llvm_symbolizer_unavailable) {
    if (auto symbolizer = CreateIndexSymbolizer(config); symbolizer)
      SymbolizePendingAddresses(groups, env, symbolizer.get(), &mappings,
                                &result.symbols);
    if (auto symbolizer = CreateFindSymbolizer(config); symbolizer)
      SymbolizePendingAddresses(groups, env, symbolizer.get(), &mappings,
                                &result.symbols);
  }
  for (const auto& path : config.breakpad_paths) {
    BreakpadSymbolizer symbolizer(path);
    SymbolizePendingAddresses(groups, env, &symbolizer, &mappings,
                              &result.symbols);
  }
  bool unresolved = false;
  for (const auto& mapping : mappings.mappings) {
    for (auto it = mapping.addresses.GetIterator(); it; ++it)
      unresolved |= !it.value().resolved;
  }
  if (unresolved && !config.debuginfod.urls.empty()) {
    auto symbolizer =
        CreateDebuginfodSymbolizer(config.debuginfod, &result.debuginfod);
    if (symbolizer) {
      result.debuginfod.enabled = true;
      SymbolizePendingAddresses(groups, env, symbolizer.get(), &mappings,
                                &result.symbols);
    }
  }
  CollectResults(mappings, &result);

  if (result.llvm_symbolizer_unavailable &&
      result.successful_mappings.empty()) {
    result.error = SymbolizerError::kSymbolizerNotAvailable;
    result.error_details = LlvmSymbolizerUnavailableMessage();
    return result;
  }

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
  uint64_t resolved = 0;
  for (const auto& mapping : result.successful_mappings)
    resolved += mapping.frame_count;
  auto groups = GroupUnresolved(result);
  uint64_t unresolved = 0;
  for (const auto& group : groups)
    unresolved += group.frame_count;
  uint64_t total = resolved + unresolved;

  std::string summary = Colorize(colorize, kBold, "Symbolization:") + " ";
  if (total == 0) {
    summary += "no native frames to symbolize.\n";
    return summary;
  }
  if (unresolved == 0) {
    summary +=
        Colorize(colorize, kGreen, "all " + Frames(resolved) + " symbolized") +
        ".\n";
  } else {
    std::string done =
        std::to_string(resolved) + " of " + Frames(total) + " symbolized";
    summary +=
        (resolved ? Colorize(colorize, kGreen, done) : done) + ", " +
        Colorize(colorize, kYellow,
                 std::to_string(unresolved) + " could not be symbolized") +
        ".\n";
  }
  if (result.llvm_symbolizer_unavailable)
    summary += Colorize(colorize, kYellow, LlvmSymbolizerUnavailableMessage());
  FormatDebuginfod(colorize, result.debuginfod, &summary);

  if (verbose) {
    FormatSuccessfulMappings(colorize, result.successful_mappings, &summary);
    FormatUnresolvedMappings(colorize, result.llvm_symbolizer_unavailable,
                             groups, &summary);
    if (unresolved) {
      summary += "\n";
      FormatFixAdvice(colorize, result.llvm_symbolizer_unavailable,
                      result.debuginfod.enabled, groups, &summary);
    }
    return summary;
  }
  if (unresolved == 0)
    return summary;
  FormatUnresolvedSummary(colorize, result.llvm_symbolizer_unavailable,
                          result.debuginfod, groups, &summary);
  FormatFixAdvice(colorize, result.llvm_symbolizer_unavailable,
                  result.debuginfod.enabled, groups, &summary);
  summary += Colorize(colorize, kGray,
                      "Run with --verbose to see build IDs and every path "
                      "searched.") +
             "\n";
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
