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

#ifndef SRC_TRACE_PROCESSOR_UTIL_TRACE_ENRICHMENT_DISASSEMBLY_H_
#define SRC_TRACE_PROCESSOR_UTIL_TRACE_ENRICHMENT_DISASSEMBLY_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace perfetto::trace_processor {
class TraceProcessor;
}

namespace perfetto::trace_processor::util {

// A function in a binary's symbol table. Addresses are link-time virtual
// addresses, i.e. the ones llvm tools print.
struct FunctionRange {
  std::string name;
  uint64_t start = 0;
  uint64_t size = 0;
};

struct DisassembledInstruction {
  uint64_t address = 0;

  // Raw encoded bytes of the instruction.
  std::string bytes;

  // Mnemonic and operands.
  std::string text;

  // For direct branches and calls, the target address and, when the
  // disassembler named it, the symbol at the target.
  std::optional<uint64_t> target_address;
  std::string target_symbol;

  // Source location, empty / zero when unknown.
  std::string source_file;
  uint32_t line_number = 0;
};

struct DisassembledFunction {
  FunctionRange range;
  std::vector<DisassembledInstruction> instructions;
};

// Parses the output of `llvm-nm -S --defined-only --numeric-sort`, keeping
// code symbols. Symbols without a size extend to the next symbol; the last
// symbol keeps size 0 until ExtendTrailingFunction sees the section it is
// in. The result is sorted by start address.
std::vector<FunctionRange> ParseNmOutput(const std::string& output);

// A code section of a binary, from `llvm-objdump --section-headers`.
struct SectionRange {
  std::string name;
  uint64_t start = 0;
  uint64_t size = 0;
};

// Parses the output of `llvm-objdump --section-headers`, keeping the code
// sections.
std::vector<SectionRange> ParseSectionHeaders(const std::string& output);

// Gives functions without a size (the last symbol of a file) the extent up to
// the end of the section containing them, and drops those in no section.
void ExtendTrailingFunction(std::vector<FunctionRange>* functions,
                            const std::vector<SectionRange>& sections);

// Returns the functions in |functions| (sorted by start address) which contain
// any of |addresses|, each once, in address order.
std::vector<FunctionRange> FunctionsContaining(
    const std::vector<FunctionRange>& functions,
    const std::vector<uint64_t>& addresses);

// Parses the output of `llvm-objdump -d -l --print-imm-hex` for one function.
std::vector<DisassembledInstruction> ParseObjdumpOutput(
    const std::string& output);

// Whether |instructions| plausibly decode real code. Disassembling a file
// which holds only debug info yields nothing for ELF but, for Mach-O, decodes
// the container's bytes as instructions, most of which are invalid.
bool LooksLikeCode(const std::vector<DisassembledInstruction>& instructions);

struct DisassemblerConfig {
  std::string llvm_nm = "llvm-nm";
  std::string llvm_objdump = "llvm-objdump";

  // Functions larger than this are not disassembled.
  uint64_t max_function_size = uint64_t{128} << 10;
};

// Disassembles functions of binaries on disk using llvm-nm and llvm-objdump.
class Disassembler {
 public:
  explicit Disassembler(DisassemblerConfig config);

  // Disassembles the functions of |code_binary| which contain any of
  // |addresses| (link-time virtual addresses), taking symbols and source
  // lines from |debug_binary| as well, which may be the same file or a split
  // debug file (a .dSYM bundle member or an ELF holding only debug info).
  // Functions with no code, e.g. when |code_binary| holds only debug info,
  // are omitted. Returns nullopt if the tools could not be run.
  std::optional<std::vector<DisassembledFunction>> Disassemble(
      const std::string& code_binary,
      const std::string& debug_binary,
      const std::vector<uint64_t>& addresses);

 private:
  DisassemblerConfig config_;
};

// A binary on disk together with the mapping it was loaded as in the trace.
struct DisassemblyBinary {
  std::string mapping_name;
  // Raw (not hex encoded) build id.
  std::string build_id;
  std::string binary_path;
  // Value to add to a mapping-relative address to obtain its link-time
  // virtual address in |binary_path|.
  uint64_t address_correction = 0;
};

struct DisassemblyResult {
  // Serialized TracePacket protos containing ModuleDisassembly packets.
  // Ready to be appended to the trace or included in a bundle.
  std::string packets;

  uint32_t function_count = 0;
  uint32_t instruction_count = 0;

  // Binaries in which no sampled function had code, e.g. files holding only
  // debug info.
  std::vector<std::string> binaries_without_code;

  // True if llvm-nm or llvm-objdump could not be run.
  bool tools_unavailable = false;
};

// For each of |binaries|, disassembles the functions containing the addresses
// of the frames in |tp| that belong to its mapping.
DisassemblyResult BundleDisassembly(
    TraceProcessor* tp,
    const std::vector<DisassemblyBinary>& binaries,
    const DisassemblerConfig& config);

// Formats a human-readable summary of |result|. Returns an empty string if
// there was nothing to disassemble.
std::string FormatDisassemblySummary(const DisassemblyResult& result,
                                     bool verbose);

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_TRACE_ENRICHMENT_DISASSEMBLY_H_
