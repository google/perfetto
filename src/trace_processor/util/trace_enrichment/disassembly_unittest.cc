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

#include <string>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {

using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;

constexpr char kNmOutput[] =
    "0000000000401000 0000000000000006 T helper\n"
    "0000000000401010 0000000000000060 T compute\n"
    "0000000000401070 t local_helper\n"
    "0000000000401080 0000000000000052 W weak_fn\n"
    "0000000000404000 0000000000000004 D data\n"
    "0000000000401078 0000000000000000 t empty_fn\n";

TEST(DisassemblyTest, ParseNmOutputKeepsCodeSymbolsInAddressOrder) {
  std::vector<FunctionRange> functions = ParseNmOutput(kNmOutput);
  EXPECT_THAT(functions,
              ElementsAre(Field(&FunctionRange::name, "helper"),
                          Field(&FunctionRange::name, "compute"),
                          Field(&FunctionRange::name, "local_helper"),
                          Field(&FunctionRange::name, "empty_fn"),
                          Field(&FunctionRange::name, "weak_fn")));
  EXPECT_EQ(functions[1].start, 0x401010u);
  EXPECT_EQ(functions[1].size, 0x60u);
  // Symbols without a size extend to the next symbol.
  EXPECT_EQ(functions[2].size, 0x8u);
  EXPECT_EQ(functions[3].size, 0x8u);
}

TEST(DisassemblyTest, ParseNmOutputKeepsTrailingSymbolWithoutSize) {
  std::vector<FunctionRange> functions =
      ParseNmOutput("0000000000401000 T only\n");
  ASSERT_EQ(functions.size(), 1u);
  EXPECT_EQ(functions[0].size, 0u);
}

// Output of `llvm-objdump --section-headers`.
constexpr char kSectionHeaders[] =
    "\n"
    "t:\tfile format mach-o arm64\n"
    "\n"
    "Sections:\n"
    "Idx Name          Size     VMA              Type\n"
    "  0 __text        00000090 0000000100000328 TEXT\n"
    "  1 __unwind_info 00000060 00000001000003b8 DATA\n"
    "  2 .text         00001000 0000000000401000 TEXT, BSS\n";

TEST(DisassemblyTest, ParseSectionHeadersKeepsCodeSections) {
  std::vector<SectionRange> sections = ParseSectionHeaders(kSectionHeaders);
  EXPECT_THAT(sections, ElementsAre(Field(&SectionRange::name, "__text"),
                                    Field(&SectionRange::name, ".text")));
  EXPECT_EQ(sections[0].start, 0x100000328u);
  EXPECT_EQ(sections[0].size, 0x90u);
}

TEST(DisassemblyTest, ExtendTrailingFunctionUsesSectionEnd) {
  std::vector<FunctionRange> functions = ParseNmOutput(
      "0000000100000328 0000000000000000 T _helper\n"
      "0000000100000334 0000000000000000 T _compute\n"
      "00000001000003b4 0000000000000000 T _main\n"
      "0000000200000000 0000000000000000 T _orphan\n");
  ExtendTrailingFunction(&functions, ParseSectionHeaders(kSectionHeaders));
  EXPECT_THAT(functions, ElementsAre(Field(&FunctionRange::name, "_helper"),
                                     Field(&FunctionRange::name, "_compute"),
                                     Field(&FunctionRange::name, "_main")));
  // _main extends to the end of __text (0x100000328 + 0x90).
  EXPECT_EQ(functions[2].size, 0x4u);
}

TEST(DisassemblyTest, FunctionsContainingDedupesAndOrders) {
  std::vector<FunctionRange> functions = ParseNmOutput(kNmOutput);
  // Two addresses in compute, one in weak_fn, one in local_helper, one in a
  // gap after weak_fn and one before all functions.
  std::vector<FunctionRange> found = FunctionsContaining(
      functions, {0x401090, 0x401020, 0x40106f, 0x401072, 0x4010e0, 0x100});
  EXPECT_THAT(found, ElementsAre(Field(&FunctionRange::name, "compute"),
                                 Field(&FunctionRange::name, "local_helper"),
                                 Field(&FunctionRange::name, "weak_fn")));
}

// Output of `llvm-objdump -d -l --print-imm-hex --x86-asm-syntax=intel`.
constexpr char kX86Output[] =
    "\n"
    "t.o:\tfile format elf64-x86-64\n"
    "\n"
    "Disassembly of section .text:\n"
    "\n"
    "0000000000401010 <compute>:\n"
    "; compute():\n"
    "; /src/t.c:4\n"
    "  401010: 85 ff                        \ttest\tedi, edi\n"
    "  401012: 7e 59                        \tjle\t0x40106d <compute+0x5d>\n"
    "; /src/t.c:5\n"
    "  401014: e8 e7 ff ff ff               \tcall\t0x401000 <helper>\n"
    "  401019: 48 8d 05 e0 2f 00 00         \tlea\trax, [rip + 0x2fe0]"
    "  # 0x404000 <data>\n"
    "; /src/t.c:8\n"
    "  401020: c3                           \tret\n";

TEST(DisassemblyTest, ParseObjdumpOutputX86) {
  std::vector<DisassembledInstruction> insns = ParseObjdumpOutput(kX86Output);
  ASSERT_EQ(insns.size(), 5u);

  EXPECT_EQ(insns[0].address, 0x401010u);
  EXPECT_EQ(insns[0].bytes, std::string("\x85\xff", 2));
  EXPECT_EQ(insns[0].text, "test edi, edi");
  EXPECT_FALSE(insns[0].target_address.has_value());
  EXPECT_EQ(insns[0].source_file, "/src/t.c");
  EXPECT_EQ(insns[0].line_number, 4u);

  EXPECT_EQ(insns[1].text, "jle 0x40106d <compute+0x5d>");
  EXPECT_EQ(insns[1].target_address, 0x40106du);
  EXPECT_EQ(insns[1].target_symbol, "compute");

  EXPECT_EQ(insns[2].bytes, std::string("\xe8\xe7\xff\xff\xff", 5));
  EXPECT_EQ(insns[2].target_address, 0x401000u);
  EXPECT_EQ(insns[2].target_symbol, "helper");
  EXPECT_EQ(insns[2].line_number, 5u);

  // A RIP-relative data reference is annotated like a branch target but is
  // not one.
  EXPECT_FALSE(insns[3].target_address.has_value());

  EXPECT_EQ(insns[4].text, "ret");
  EXPECT_EQ(insns[4].line_number, 8u);
}

// Output of `llvm-objdump -d -l --print-imm-hex` for arm64, where each
// instruction is printed as one 32-bit word.
constexpr char kArm64Output[] =
    "000000000040000c <compute>:\n"
    "; compute():\n"
    "; /src/t.c:4\n"
    "  40000c: 7100041f     \tcmp\tw0, #0x1\n"
    "  400010: 540003ab     \tb.lt\t0x400084 <compute+0x78>\n"
    "  400024: 52807d08     \tmov\tw8, #0x3e8              // =1000\n"
    "  400064: 94000000     \tbl\t0x400000 <helper>\n"
    "  400070: d65f03c0     \tret\n";

TEST(DisassemblyTest, ParseObjdumpOutputArm64) {
  std::vector<DisassembledInstruction> insns = ParseObjdumpOutput(kArm64Output);
  ASSERT_EQ(insns.size(), 5u);
  // Words are converted to little-endian memory order.
  EXPECT_EQ(insns[0].bytes, std::string("\x1f\x04\x00\x71", 4));
  EXPECT_EQ(insns[1].text, "b.lt 0x400084 <compute+0x78>");
  EXPECT_EQ(insns[1].target_address, 0x400084u);
  EXPECT_EQ(insns[2].text, "mov w8, #0x3e8");
  EXPECT_EQ(insns[3].target_symbol, "helper");
  EXPECT_EQ(insns[4].bytes, std::string("\xc0\x03\x5f\xd6", 4));
}

TEST(DisassemblyTest, ParseObjdumpOutputStripsMachOComments) {
  std::vector<DisassembledInstruction> insns =
      ParseObjdumpOutput("100000358: 087d8052    \tmov\tw8, #0x3e8 ; =1000\n");
  ASSERT_EQ(insns.size(), 1u);
  EXPECT_EQ(insns[0].text, "mov w8, #0x3e8");
}

TEST(DisassemblyTest, LooksLikeCodeRejectsDecodedDebugInfo) {
  EXPECT_TRUE(LooksLikeCode(ParseObjdumpOutput(kArm64Output)));
  // What objdump makes of a dSYM: the container decoded as instructions.
  std::vector<DisassembledInstruction> garbage = ParseObjdumpOutput(
      "100000334: 0000000a    \tudf\t#0xa\n"
      "100000338: 00000007    \tudf\t#0x7\n"
      "10000033c: 5de226eb    \t<unknown>\n"
      "100000340: 383f637a    \tldumaxb\twzr, w26, [x27]\n");
  EXPECT_EQ(garbage.size(), 4u);
  EXPECT_FALSE(LooksLikeCode(garbage));
  EXPECT_FALSE(LooksLikeCode({}));
}

TEST(DisassemblyTest, ParseObjdumpOutputWithoutCode) {
  EXPECT_THAT(ParseObjdumpOutput("t.debug:\tfile format elf64-x86-64\n\n"),
              IsEmpty());
}

}  // namespace
}  // namespace perfetto::trace_processor::util
