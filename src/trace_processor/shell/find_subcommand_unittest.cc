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

#include "src/trace_processor/shell/subcommand.h"

#include <string>
#include <unordered_set>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/shell/traceconv_compat.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::shell {
namespace {

// A minimal Subcommand implementation for testing.
class FakeSubcommand : public Subcommand {
 public:
  explicit FakeSubcommand(const char* n) : name_(n) {}
  const char* name() const override { return name_; }
  const char* description() const override { return ""; }
  const char* usage_args() const override { return ""; }
  const char* detailed_help() const override { return ""; }
  std::vector<FlagSpec> GetFlags() override { return {}; }
  base::Status Run(const SubcommandContext&) override {
    return base::OkStatus();
  }

 private:
  const char* name_;
};

// Helper to build an argv array from an initializer list.
struct ArgvHolder {
  std::vector<std::string> strings;
  std::vector<char*> ptrs;

  static ArgvHolder Make(std::initializer_list<const char*> args) {
    ArgvHolder h;
    for (const char* a : args) {
      h.strings.emplace_back(a);
    }
    for (auto& s : h.strings) {
      h.ptrs.push_back(s.data());
    }
    return h;
  }

  int argc() const { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }
};

TEST(FindSubcommandTest, EmptyArgvReturnsNull) {
  FakeSubcommand query("query");
  std::vector<Subcommand*> subs = {&query};

  auto args = ArgvHolder::Make({"tp_shell"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, {});
  EXPECT_EQ(result.subcommand, nullptr);
}

TEST(FindSubcommandTest, OnlyFlagsReturnsNull) {
  FakeSubcommand query("query");
  std::vector<Subcommand*> subs = {&query};

  auto args = ArgvHolder::Make({"tp_shell", "-v", "--full-sort"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, {});
  EXPECT_EQ(result.subcommand, nullptr);
}

TEST(FindSubcommandTest, UnknownPositionalReturnsNull) {
  FakeSubcommand query("query");
  std::vector<Subcommand*> subs = {&query};

  auto args = ArgvHolder::Make({"tp_shell", "trace.pb"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, {});
  EXPECT_EQ(result.subcommand, nullptr);
}

TEST(FindSubcommandTest, KnownSubcommandReturnsPtr) {
  FakeSubcommand query("query");
  FakeSubcommand serve("serve");
  std::vector<Subcommand*> subs = {&query, &serve};

  auto args = ArgvHolder::Make({"tp_shell", "query", "-c", "SELECT 1"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, {});
  EXPECT_EQ(result.subcommand, &query);
  EXPECT_EQ(result.argv_index, 1);
}

TEST(FindSubcommandTest, FlagWithArgSkipsValue) {
  FakeSubcommand query("query");
  std::vector<Subcommand*> subs = {&query};

  // --dev-flag takes an argument "x=y", so "query" at index 3 should be found.
  auto args =
      ArgvHolder::Make({"tp_shell", "--dev-flag", "x=y", "query", "trace.pb"});
  std::unordered_set<std::string> fwa = {"--dev-flag"};
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, fwa);
  EXPECT_EQ(result.subcommand, &query);
  EXPECT_EQ(result.argv_index, 3);
}

TEST(FindSubcommandTest, SubcommandAfterFlags) {
  FakeSubcommand query("query");
  std::vector<Subcommand*> subs = {&query};

  auto args =
      ArgvHolder::Make({"tp_shell", "--dev", "query", "-c", "sql", "trace.pb"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, {});
  EXPECT_EQ(result.subcommand, &query);
  EXPECT_EQ(result.argv_index, 2);
}

// ---------------------------------------------------------------------------
// traceconv compatibility shim.
// ---------------------------------------------------------------------------

TEST(InvokedAsTraceconvTest, BareNameMatches) {
  EXPECT_TRUE(InvokedAsTraceconv("traceconv"));
}

TEST(InvokedAsTraceconvTest, WithDirectoryMatches) {
  EXPECT_TRUE(InvokedAsTraceconv("/usr/local/bin/traceconv"));
  EXPECT_TRUE(InvokedAsTraceconv("./traceconv"));
}

TEST(InvokedAsTraceconvTest, WindowsExeMatches) {
  EXPECT_TRUE(InvokedAsTraceconv("traceconv.exe"));
  EXPECT_TRUE(InvokedAsTraceconv("C:\\tools\\traceconv.exe"));
}

TEST(InvokedAsTraceconvTest, PrebuiltCacheNameMatches) {
  // The prebuilt wrapper caches the binary as "traceconv-<sha16>".
  EXPECT_TRUE(InvokedAsTraceconv(
      "/home/u/.local/share/perfetto/prebuilts/traceconv-0123456789abcdef"));
  EXPECT_TRUE(InvokedAsTraceconv("traceconv-0123456789abcdef.exe"));
}

TEST(InvokedAsTraceconvTest, TraceProcessorDoesNotMatch) {
  EXPECT_FALSE(InvokedAsTraceconv("trace_processor_shell"));
  EXPECT_FALSE(InvokedAsTraceconv("/opt/perfetto/trace_processor_shell"));
  EXPECT_FALSE(InvokedAsTraceconv("tp"));
}

TEST(InvokedAsTraceconvTest, SimilarNamesDoNotMatch) {
  // A binary whose name merely starts with the letters "traceconv" but is a
  // different tool must not be hijacked into traceconv-compatible mode.
  EXPECT_FALSE(InvokedAsTraceconv("traceconverter"));
  EXPECT_FALSE(InvokedAsTraceconv("my_traceconv"));
}

// Helper: turns the rewritten arg vector into a single space-joined string for
// concise assertions.
std::string Join(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) {
    if (!out.empty())
      out += ' ';
    out += s;
  }
  return out;
}

TEST(RewriteTraceconvArgsTest, ConvertFormatGetsConvertInserted) {
  auto args = ArgvHolder::Make({"traceconv", "json", "in.pb", "out.json"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv convert json in.pb out.json");
}

TEST(RewriteTraceconvArgsTest, ProfileFormatGetsConvertInserted) {
  auto args = ArgvHolder::Make({"traceconv", "profile", "in.pb"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv convert profile in.pb");
}

TEST(RewriteTraceconvArgsTest, SymbolizeGetsUtilInserted) {
  auto args = ArgvHolder::Make({"traceconv", "symbolize", "in.pb", "out"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv util symbolize in.pb out");
}

TEST(RewriteTraceconvArgsTest, DeobfuscateGetsUtilInserted) {
  auto args = ArgvHolder::Make({"traceconv", "deobfuscate", "in.pb"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv util deobfuscate in.pb");
}

TEST(RewriteTraceconvArgsTest, BundleIsLeftUnchanged) {
  // "bundle" is itself a subcommand name; no word is inserted.
  auto args = ArgvHolder::Make({"traceconv", "bundle", "in.pb", "out.tar"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_TRUE(out.empty());
}

TEST(RewriteTraceconvArgsTest, DecompressPacketsGetsUtilInserted) {
  auto args =
      ArgvHolder::Make({"traceconv", "decompress_packets", "in.pb", "out.pb"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv util decompress_packets in.pb out.pb");
}

TEST(RewriteTraceconvArgsTest, BinaryIsRenamedToUtilTextToBinary) {
  // The legacy "binary" mode moved to "util text_to_binary".
  auto args = ArgvHolder::Make({"traceconv", "binary", "in.txt", "out.pb"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv util text_to_binary in.txt out.pb");
}

TEST(RewriteTraceconvArgsTest, JavaHeapProfileBecomesConvertProfileJavaHeap) {
  // The legacy "java_heap_profile" alias became "convert profile --java-heap".
  auto args = ArgvHolder::Make(
      {"traceconv", "java_heap_profile", "in.pb", "--output-dir", "/tmp/x"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out),
            "traceconv convert profile --java-heap in.pb --output-dir /tmp/x");
}

TEST(RewriteTraceconvArgsTest, UnknownModeRoutesToConvert) {
  // An unrecognised MODE is routed to convert, which emits a clear error.
  auto args = ArgvHolder::Make({"traceconv", "wibble", "in.pb"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv convert wibble in.pb");
}

TEST(RewriteTraceconvArgsTest, NoPositionalLeavesArgsUnchanged) {
  // Bare invocation and version/help flags have no MODE positional.
  auto bare = ArgvHolder::Make({"traceconv"});
  EXPECT_TRUE(RewriteTraceconvArgs(bare.argc(), bare.argv()).empty());

  auto version = ArgvHolder::Make({"traceconv", "--version"});
  EXPECT_TRUE(RewriteTraceconvArgs(version.argc(), version.argv()).empty());
}

TEST(RewriteTraceconvArgsTest, ValuedFlagBeforeModeSkipsItsArgument) {
  // -t consumes "end"; the MODE is "json", which must still be found and the
  // leading flags preserved ahead of the inserted subcommand word.
  auto args =
      ArgvHolder::Make({"traceconv", "-t", "end", "json", "in.pb", "out.json"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv -t end convert json in.pb out.json");
}

TEST(RewriteTraceconvArgsTest, LongValuedFlagBeforeModeSkipsItsArgument) {
  auto args =
      ArgvHolder::Make({"traceconv", "--pid", "1234", "profile", "in.pb"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv --pid 1234 convert profile in.pb");
}

TEST(RewriteTraceconvArgsTest, BooleanFlagBeforeModeIsNotTreatedAsValued) {
  // --full-sort takes no argument, so the very next token ("json") is the MODE.
  auto args = ArgvHolder::Make({"traceconv", "--full-sort", "json", "in.pb"});
  auto out = RewriteTraceconvArgs(args.argc(), args.argv());
  EXPECT_EQ(Join(out), "traceconv --full-sort convert json in.pb");
}

}  // namespace
}  // namespace perfetto::trace_processor::shell
