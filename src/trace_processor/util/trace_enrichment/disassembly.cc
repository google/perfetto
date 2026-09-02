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

#include "src/trace_processor/util/trace_enrichment/disassembly.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/util/build_id.h"

#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

// Running llvm-nm and llvm-objdump needs subprocesses, which are only
// available where the local symbolizer is (not in WASM builds).
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "src/trace_processor/util/symbolizer/local_symbolizer.h"
#endif

namespace perfetto::trace_processor::util {
namespace {

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

std::string Trim(std::string_view s) {
  return std::string(base::TrimWhitespace(s));
}

std::vector<std::string> SplitWhitespace(const std::string& s) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
      i++;
    }
    size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') {
      i++;
    }
    if (i > start) {
      tokens.push_back(s.substr(start, i - start));
    }
  }
  return tokens;
}

std::optional<uint8_t> HexByte(std::string_view s) {
  if (s.size() != 2) {
    return std::nullopt;
  }
  std::optional<uint64_t> value = base::StringToUInt64(std::string(s), 16);
  if (!value) {
    return std::nullopt;
  }
  return static_cast<uint8_t>(*value);
}

// Decodes the byte column of an objdump line. x86 prints one token per byte;
// fixed-width ISAs print each instruction word as a single number, which
// needs to be converted back to memory (little-endian) order.
std::string DecodeBytes(const std::vector<std::string>& tokens) {
  std::string bytes;
  if (tokens.size() == 1 && tokens[0].size() > 2 && tokens[0].size() % 2 == 0) {
    const std::string& word = tokens[0];
    for (size_t i = word.size(); i >= 2; i -= 2) {
      std::optional<uint8_t> byte =
          HexByte(std::string_view(word).substr(i - 2, 2));
      if (!byte) {
        return "";
      }
      bytes.push_back(static_cast<char>(*byte));
    }
    return bytes;
  }
  for (const std::string& token : tokens) {
    std::optional<uint8_t> byte = HexByte(token);
    if (!byte) {
      return "";
    }
    bytes.push_back(static_cast<char>(*byte));
  }
  return bytes;
}

// Whether |mnemonic| transfers control to a statically known address, so
// that a `0x... <symbol>` operand denotes its target rather than, e.g., the
// data a RIP-relative load refers to.
bool IsDirectBranch(const std::string& mnemonic) {
  if (mnemonic.empty()) {
    return false;
  }
  // x86.
  if (mnemonic[0] == 'j' || mnemonic == "call" ||
      base::StartsWith(mnemonic, "loop")) {
    return true;
  }
  // arm64 / arm.
  if (mnemonic == "b" || mnemonic == "bl" || base::StartsWith(mnemonic, "b.") ||
      mnemonic == "cbz" || mnemonic == "cbnz" || mnemonic == "tbz" ||
      mnemonic == "tbnz") {
    return true;
  }
  return false;
}

// Parses a `0x<addr> <symbol[+0x<off>]>` operand.
void ParseBranchTarget(const std::string& operands,
                       DisassembledInstruction* insn) {
  size_t lt = operands.find('<');
  size_t gt = operands.find('>', lt == std::string::npos ? 0 : lt);
  if (lt == std::string::npos || gt == std::string::npos) {
    return;
  }
  // The address is the token immediately before the '<'.
  std::string before = Trim(std::string_view(operands).substr(0, lt));
  size_t space = before.find_last_of(" ,");
  std::string address_str =
      space == std::string::npos ? before : before.substr(space + 1);
  if (!base::StartsWith(address_str, "0x")) {
    return;
  }
  std::optional<uint64_t> address =
      base::StringToUInt64(address_str.substr(2), 16);
  if (!address) {
    return;
  }
  insn->target_address = *address;
  std::string symbol = operands.substr(lt + 1, gt - lt - 1);
  size_t plus = symbol.find('+');
  if (plus != std::string::npos) {
    symbol = symbol.substr(0, plus);
  }
  insn->target_symbol = symbol;
}

// Runs |exe| with |args| and returns its stdout, or nullopt if it could not
// be run or failed.
std::optional<std::string> RunTool(const std::string& exe,
                                   std::vector<std::string> args) {
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
  constexpr int kToolTimeoutMs = 120 * 1000;
  base::Subprocess process;
  process.args.exec_cmd.push_back(exe);
  for (std::string& arg : args) {
    process.args.exec_cmd.push_back(std::move(arg));
  }
  process.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.stderr_mode = base::Subprocess::OutputMode::kDevNull;
  if (!process.Call(kToolTimeoutMs)) {
    return std::nullopt;
  }
  return std::move(process.output());
#else
  base::ignore_result(exe, args);
  return std::nullopt;
#endif
}

// objdump arguments pointing it at |debug_binary| for line information when
// that is not the binary being disassembled: the .dSYM bundle on Mach-O, the
// directory holding the split debug file on ELF (found via .gnu_debuglink).
std::vector<std::string> DebugInfoArgs(const std::string& code_binary,
                                       const std::string& debug_binary) {
  if (debug_binary.empty() || debug_binary == code_binary) {
    return {};
  }
  size_t dsym = debug_binary.find(".dSYM/");
  if (dsym != std::string::npos) {
    return {"--dsym=" + debug_binary.substr(0, dsym + 5)};
  }
  size_t slash = debug_binary.find_last_of('/');
  if (slash == std::string::npos) {
    return {};
  }
  return {"--debug-file-directory=" + debug_binary.substr(0, slash)};
}

// |mapping_name| if it is a file on this machine with build id |build_id|
// (raw), i.e. the very binary the trace was recorded with; empty otherwise.
std::string VerifiedCodeBinary(const std::string& mapping_name,
                               const std::string& build_id) {
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
  if (mapping_name.empty() || mapping_name[0] != '/' ||
      !base::FileExists(mapping_name)) {
    return "";
  }
  profiling::LocalBinaryIndexer indexer({}, {mapping_name});
  profiling::BinaryLookupResult lookup =
      indexer.FindBinary(mapping_name, build_id);
  return lookup.ok() ? lookup.binary->file_name : "";
#else
  base::ignore_result(mapping_name, build_id);
  return "";
#endif
}

std::string Hex(uint64_t value) {
  return "0x" + base::Uint64ToHexStringNoPrefix(value);
}

std::string SqlQuote(const std::string& value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') {
      out += "''";
    } else {
      out.push_back(c);
    }
  }
  out += "'";
  return out;
}

}  // namespace

std::vector<FunctionRange> ParseNmOutput(const std::string& output) {
  std::vector<FunctionRange> functions;
  for (const std::string& line : SplitLines(output)) {
    // `<address> <size> <type> <name>`, or `<address> <type> <name>` for
    // symbols without a size.
    std::vector<std::string> tokens = SplitWhitespace(line);
    if (tokens.size() < 3) {
      continue;
    }
    bool has_size = tokens.size() >= 4;
    const std::string& type = tokens[has_size ? 2 : 1];
    if (type != "t" && type != "T" && type != "w" && type != "W") {
      continue;
    }
    std::optional<uint64_t> start = base::StringToUInt64(tokens[0], 16);
    if (!start) {
      continue;
    }
    uint64_t size = 0;
    if (has_size) {
      size = base::StringToUInt64(tokens[1], 16).value_or(0);
    }
    functions.push_back({tokens[has_size ? 3 : 2], *start, size});
  }
  std::sort(functions.begin(), functions.end(),
            [](const FunctionRange& a, const FunctionRange& b) {
              return a.start < b.start;
            });
  // Symbol tables do not always carry sizes (Mach-O never does, ELF omits
  // them for symbols defined in assembly). Take a function to extend to the
  // next symbol in that case.
  for (size_t i = 0; i + 1 < functions.size(); i++) {
    if (functions[i].size == 0) {
      functions[i].size = functions[i + 1].start - functions[i].start;
    }
  }
  return functions;
}

std::vector<SectionRange> ParseSectionHeaders(const std::string& output) {
  std::vector<SectionRange> sections;
  for (const std::string& line : SplitLines(output)) {
    // `<idx> <name> <size> <vma> <type flags...>`.
    std::vector<std::string> tokens = SplitWhitespace(line);
    if (tokens.size() < 5) {
      continue;
    }
    bool is_code = false;
    for (size_t i = 4; i < tokens.size(); i++) {
      if (tokens[i] == "TEXT" || tokens[i] == "TEXT,") {
        is_code = true;
      }
    }
    std::optional<uint64_t> size = base::StringToUInt64(tokens[2], 16);
    std::optional<uint64_t> start = base::StringToUInt64(tokens[3], 16);
    if (!is_code || !size || !start || *size == 0) {
      continue;
    }
    sections.push_back({tokens[1], *start, *size});
  }
  return sections;
}

void ExtendTrailingFunction(std::vector<FunctionRange>* functions,
                            const std::vector<SectionRange>& sections) {
  for (FunctionRange& function : *functions) {
    for (const SectionRange& section : sections) {
      uint64_t end = section.start + section.size;
      if (function.start < section.start || function.start >= end) {
        continue;
      }
      // A size inferred from the next symbol must not cross into another
      // section, and the last symbol of a section extends to its end.
      if (function.size == 0 || function.start + function.size > end) {
        function.size = end - function.start;
      }
      break;
    }
  }
  functions->erase(
      std::remove_if(functions->begin(), functions->end(),
                     [](const FunctionRange& f) { return f.size == 0; }),
      functions->end());
}

std::vector<FunctionRange> FunctionsContaining(
    const std::vector<FunctionRange>& functions,
    const std::vector<uint64_t>& addresses) {
  std::map<uint64_t, FunctionRange> found;
  for (uint64_t address : addresses) {
    // The last function starting at or before the address.
    auto it = std::upper_bound(
        functions.begin(), functions.end(), address,
        [](uint64_t a, const FunctionRange& f) { return a < f.start; });
    if (it == functions.begin()) {
      continue;
    }
    --it;
    if (address >= it->start + it->size) {
      continue;
    }
    found.emplace(it->start, *it);
  }
  std::vector<FunctionRange> result;
  for (auto& [start, function] : found) {
    result.push_back(std::move(function));
  }
  return result;
}

std::vector<DisassembledInstruction> ParseObjdumpOutput(
    const std::string& output) {
  std::vector<DisassembledInstruction> instructions;
  std::string source_file;
  uint32_t line_number = 0;
  for (const std::string& raw_line : SplitLines(output)) {
    std::string line = Trim(raw_line);
    if (line.empty()) {
      continue;
    }
    // `-l` emits the location of the following instructions as comments:
    // `; func():` for the function and `; /path/file.cc:12` for lines.
    if (line[0] == ';') {
      std::string comment = Trim(std::string_view(line).substr(1));
      size_t colon = comment.find_last_of(':');
      if (base::EndsWith(comment, "():") || colon == std::string::npos) {
        continue;
      }
      std::optional<uint64_t> number =
          base::StringToUInt64(comment.substr(colon + 1), 10);
      if (!number) {
        continue;
      }
      source_file = comment.substr(0, colon);
      line_number = static_cast<uint32_t>(*number);
      continue;
    }
    // Instruction lines are `<addr>: <bytes>\t<mnemonic>\t<operands>`.
    size_t colon = raw_line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::optional<uint64_t> address = base::StringToUInt64(
        Trim(std::string_view(raw_line).substr(0, colon)), 16);
    if (!address) {
      continue;
    }
    std::vector<std::string> columns;
    size_t start = colon + 1;
    while (start <= raw_line.size()) {
      size_t tab = raw_line.find('\t', start);
      if (tab == std::string::npos) {
        tab = raw_line.size();
      }
      columns.push_back(raw_line.substr(start, tab - start));
      start = tab + 1;
    }
    if (columns.size() < 2) {
      continue;
    }
    DisassembledInstruction insn;
    insn.address = *address;
    insn.bytes = DecodeBytes(SplitWhitespace(columns[0]));
    if (insn.bytes.empty()) {
      continue;
    }
    std::string mnemonic = Trim(columns[1]);
    std::string operands = columns.size() > 2 ? Trim(columns[2]) : "";
    // Drop trailing annotations such as the decimal value of an immediate,
    // introduced by `//` (ELF) or `;` (Mach-O).
    size_t comment = std::min(operands.find("//"), operands.find(" ;"));
    if (comment != std::string::npos) {
      operands = Trim(std::string_view(operands).substr(0, comment));
    }
    insn.text = operands.empty() ? mnemonic : mnemonic + " " + operands;
    if (IsDirectBranch(mnemonic)) {
      ParseBranchTarget(operands, &insn);
    }
    insn.source_file = source_file;
    insn.line_number = line_number;
    instructions.push_back(std::move(insn));
  }
  return instructions;
}

bool LooksLikeCode(const std::vector<DisassembledInstruction>& instructions) {
  if (instructions.empty()) {
    return false;
  }
  size_t invalid = 0;
  for (const DisassembledInstruction& insn : instructions) {
    if (insn.text == "<unknown>" || base::StartsWith(insn.text, "udf ")) {
      invalid++;
    }
  }
  // Real code has the odd data-in-text word; container bytes decode as
  // invalid instructions far more often than that.
  return invalid * 10 < instructions.size();
}

Disassembler::Disassembler(DisassemblerConfig config)
    : config_(std::move(config)) {}

std::optional<std::vector<DisassembledFunction>> Disassembler::Disassemble(
    const std::string& code_binary,
    const std::string& debug_binary,
    const std::vector<uint64_t>& addresses) {
  // Symbols may live in either file: a stripped executable keeps them only
  // in its debug file, a debug file may lack the ones added by the linker.
  std::vector<FunctionRange> functions;
  for (const std::string& binary : {debug_binary, code_binary}) {
    if (binary.empty() || (binary == code_binary &&
                           code_binary == debug_binary && !functions.empty())) {
      continue;
    }
    std::optional<std::string> nm_output = RunTool(
        config_.llvm_nm, {"-S", "--defined-only", "--numeric-sort", binary});
    if (!nm_output) {
      return std::nullopt;
    }
    for (FunctionRange& function : ParseNmOutput(*nm_output)) {
      bool known = std::any_of(
          functions.begin(), functions.end(),
          [&](const FunctionRange& f) { return f.start == function.start; });
      if (!known) {
        functions.push_back(std::move(function));
      }
    }
  }
  std::sort(functions.begin(), functions.end(),
            [](const FunctionRange& a, const FunctionRange& b) {
              return a.start < b.start;
            });
  std::optional<std::string> sections_output =
      RunTool(config_.llvm_objdump, {"--section-headers", code_binary});
  if (!sections_output) {
    return std::nullopt;
  }
  ExtendTrailingFunction(&functions, ParseSectionHeaders(*sections_output));

  std::vector<std::string> debug_args =
      DebugInfoArgs(code_binary, debug_binary);
  std::vector<DisassembledFunction> result;
  for (const FunctionRange& function :
       FunctionsContaining(functions, addresses)) {
    if (function.size > config_.max_function_size) {
      continue;
    }
    std::vector<std::string> args = {
        "-d",
        "-l",
        "--print-imm-hex",
        "--x86-asm-syntax=intel",
        "--start-address=" + Hex(function.start),
        "--stop-address=" + Hex(function.start + function.size)};
    args.insert(args.end(), debug_args.begin(), debug_args.end());
    args.push_back(code_binary);
    std::optional<std::string> objdump_output =
        RunTool(config_.llvm_objdump, std::move(args));
    if (!objdump_output) {
      return std::nullopt;
    }
    std::vector<DisassembledInstruction> instructions =
        ParseObjdumpOutput(*objdump_output);
    if (instructions.empty() || !LooksLikeCode(instructions)) {
      continue;
    }
    result.push_back({function, std::move(instructions)});
  }
  return result;
}

DisassemblyResult BundleDisassembly(
    TraceProcessor* tp,
    const std::vector<DisassemblyBinary>& binaries,
    const DisassemblerConfig& config) {
  DisassemblyResult result;
  Disassembler disassembler(config);
  for (const DisassemblyBinary& binary : binaries) {
    std::string build_id_hex = BuildId::FromRaw(binary.build_id).ToHex();
    auto it = tp->ExecuteQuery(R"(
      SELECT DISTINCT spf.rel_pc
      FROM stack_profile_frame spf
      JOIN stack_profile_mapping spm ON spf.mapping = spm.id
      WHERE spm.build_id = )" + SqlQuote(build_id_hex) +
                               " AND spm.name = " +
                               SqlQuote(binary.mapping_name));
    std::vector<uint64_t> addresses;
    while (it.Next()) {
      addresses.push_back(static_cast<uint64_t>(it.Get(0).AsLong()) +
                          binary.address_correction);
    }
    if (addresses.empty()) {
      continue;
    }

    // The symbolizer's binary may hold only debug info; the mapping's own
    // path is then the code binary if it is the same build.
    std::vector<std::string> code_candidates = {binary.binary_path};
    std::string verified =
        VerifiedCodeBinary(binary.mapping_name, binary.build_id);
    if (!verified.empty() && verified != binary.binary_path) {
      code_candidates.push_back(verified);
    }
    std::optional<std::vector<DisassembledFunction>> functions;
    for (const std::string& code_binary : code_candidates) {
      functions =
          disassembler.Disassemble(code_binary, binary.binary_path, addresses);
      if (!functions) {
        result.tools_unavailable = true;
        return result;
      }
      if (!functions->empty()) {
        break;
      }
    }
    if (functions->empty()) {
      result.binaries_without_code.push_back(binary.binary_path);
      continue;
    }

    protozero::HeapBuffered<protos::pbzero::Trace> trace;
    auto* module = trace->add_packet()->set_module_disassembly();
    module->set_path(binary.mapping_name);
    module->set_build_id(binary.build_id);
    // The source file table is a field of the module message, which cannot
    // be written to while a nested function message is open, so it is built
    // and emitted before the functions.
    std::map<std::string, uint32_t> source_file_index;
    for (const DisassembledFunction& function : *functions) {
      for (const DisassembledInstruction& insn : function.instructions) {
        if (!insn.source_file.empty() && insn.line_number != 0) {
          source_file_index.emplace(
              insn.source_file,
              static_cast<uint32_t>(source_file_index.size()));
        }
      }
    }
    std::vector<const std::string*> source_files(source_file_index.size());
    for (const auto& [path, index] : source_file_index) {
      source_files[index] = &path;
    }
    for (const std::string* path : source_files) {
      module->add_source_files(*path);
    }
    for (const DisassembledFunction& function : *functions) {
      auto* fn = module->add_functions();
      fn->set_name(function.range.name);
      fn->set_start_address(function.range.start - binary.address_correction);
      fn->set_size(function.range.size);
      for (const DisassembledInstruction& insn : function.instructions) {
        auto* out = fn->add_instructions();
        out->set_address(insn.address - binary.address_correction);
        out->set_bytes(insn.bytes);
        out->set_text(insn.text);
        if (insn.target_address) {
          out->set_target_address(*insn.target_address -
                                  binary.address_correction);
          bool in_function =
              *insn.target_address >= function.range.start &&
              *insn.target_address < function.range.start + function.range.size;
          if (!in_function && !insn.target_symbol.empty()) {
            out->set_target_symbol(insn.target_symbol);
          }
        }
        if (!insn.source_file.empty() && insn.line_number != 0) {
          out->set_source_file_index(source_file_index.at(insn.source_file));
          out->set_line_number(insn.line_number);
        }
      }
      result.function_count++;
      result.instruction_count +=
          static_cast<uint32_t>(function.instructions.size());
    }
    result.packets += trace.SerializeAsString();
  }
  return result;
}

std::string FormatDisassemblySummary(const DisassemblyResult& result,
                                     bool verbose) {
  if (result.tools_unavailable) {
    return "Disassembly: skipped, llvm-nm / llvm-objdump could not be run. "
           "Install LLVM to bundle disassembly.\n";
  }
  if (result.function_count == 0 && result.binaries_without_code.empty()) {
    return "";
  }
  std::string out = "Disassembly: " + std::to_string(result.function_count) +
                    " functions (" + std::to_string(result.instruction_count) +
                    " instructions)";
  if (!result.binaries_without_code.empty()) {
    out += ", " + std::to_string(result.binaries_without_code.size()) +
           " binaries without code";
  }
  out += "\n";
  if (result.binaries_without_code.empty()) {
    return out;
  }
  if (!verbose) {
    out +=
        "  Symbol files without code cannot be disassembled; point "
        "--symbol-paths at the unstripped binaries. Use --verbose to "
        "list them.\n";
    return out;
  }
  for (const std::string& path : result.binaries_without_code) {
    out += "  - no code: " + path + "\n";
  }
  return out;
}

}  // namespace perfetto::trace_processor::util
