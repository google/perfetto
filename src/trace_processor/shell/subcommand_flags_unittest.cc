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

#include "src/trace_processor/shell/common_flags.h"

#include <initializer_list>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/trace_processor/shell/bundle_subcommand.h"
#include "src/trace_processor/shell/convert_subcommand.h"
#include "src/trace_processor/shell/subcommand.h"
#include "src/trace_processor/shell/util_subcommand.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::shell {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

// ParseFlags() fully re-initializes getopt's global scan state on each call, so
// sequential parses in one process are independent. We only silence getopt's
// stderr diagnostics so the unknown-flag test stays quiet.
void SilenceGetoptErrors() {
  opterr = 0;
}

// Owns the backing storage for an argv array built from string literals.
struct ArgvHolder {
  std::vector<std::string> strings;
  std::vector<char*> ptrs;

  static ArgvHolder Make(std::initializer_list<const char*> args) {
    ArgvHolder h;
    for (const char* a : args)
      h.strings.emplace_back(a);
    for (auto& s : h.strings)
      h.ptrs.push_back(s.data());
    return h;
  }

  int argc() const { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }
};

base::Status RunParse(Subcommand* cmd,
                      SubcommandContext* ctx,
                      std::initializer_list<const char*> args) {
  SilenceGetoptErrors();
  ArgvHolder h = ArgvHolder::Make(args);
  return ParseFlags(cmd, ctx, h.argc(), h.argv());
}

size_t CountOccurrences(const std::string& haystack,
                        const std::string& needle) {
  size_t count = 0;
  for (size_t pos = haystack.find(needle); pos != std::string::npos;
       pos = haystack.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

SubcommandContext CtxWithPositionals(std::vector<std::string> positionals) {
  SubcommandContext ctx;
  ctx.positional_args = std::move(positionals);
  return ctx;
}

base::TempFile WriteTempFile(const std::string& content) {
  base::TempFile f = base::TempFile::Create();
  PERFETTO_CHECK(base::WriteAll(f.fd(), content.data(), content.size()) ==
                 static_cast<ssize_t>(content.size()));
  return f;
}

// A subcommand with a representative mix of flags for exercising ParseFlags:
// a boolean flag, a string flag (both with short forms), and a repeatable flag.
class FlagTestSubcommand : public Subcommand {
 public:
  bool flag_a = false;
  std::string str;
  std::vector<std::string> rep;

  const char* name() const override { return "fake"; }
  const char* description() const override { return "Fake description."; }
  const char* usage_args() const override { return "<args>"; }
  const char* detailed_help() const override { return "Detailed help body."; }
  std::vector<FlagSpec> GetFlags() override {
    return {
        BoolFlag("flag-a", 'a', "Boolean flag A.", &flag_a),
        StringFlag("str", 's', "VAL", "String flag.", &str),
        FlagSpec{"rep", '\0', true, "ITEM", "Repeatable flag.",
                 [this](const char* v) { rep.emplace_back(v); }},
    };
  }
  base::Status Run(const SubcommandContext&) override {
    return base::OkStatus();
  }
};

// --- ParseFlags ---

class ParseFlagsTest : public ::testing::Test {
 protected:
  base::Status Parse(std::initializer_list<const char*> args) {
    return RunParse(&cmd_, &ctx_, args);
  }

  FlagTestSubcommand cmd_;
  GlobalOptions global_;
  SubcommandContext ctx_;

  void SetUp() override { ctx_.global = &global_; }
};

TEST_F(ParseFlagsTest, BoolLongFlag) {
  ASSERT_TRUE(Parse({"prog", "--flag-a"}).ok());
  EXPECT_TRUE(cmd_.flag_a);
}

TEST_F(ParseFlagsTest, BoolShortFlag) {
  ASSERT_TRUE(Parse({"prog", "-a"}).ok());
  EXPECT_TRUE(cmd_.flag_a);
}

TEST_F(ParseFlagsTest, BoolFlagDefaultsFalse) {
  ASSERT_TRUE(Parse({"prog"}).ok());
  EXPECT_FALSE(cmd_.flag_a);
}

TEST_F(ParseFlagsTest, StringFlagSpaceSeparated) {
  ASSERT_TRUE(Parse({"prog", "--str", "hello"}).ok());
  EXPECT_EQ(cmd_.str, "hello");
}

TEST_F(ParseFlagsTest, StringFlagEqualsSeparated) {
  ASSERT_TRUE(Parse({"prog", "--str=hello"}).ok());
  EXPECT_EQ(cmd_.str, "hello");
}

TEST_F(ParseFlagsTest, StringFlagShortForm) {
  ASSERT_TRUE(Parse({"prog", "-s", "x"}).ok());
  EXPECT_EQ(cmd_.str, "x");
}

TEST_F(ParseFlagsTest, RepeatableFlagAccumulates) {
  ASSERT_TRUE(Parse({"prog", "--rep", "1", "--rep", "2"}).ok());
  EXPECT_EQ(cmd_.rep, (std::vector<std::string>{"1", "2"}));
}

TEST_F(ParseFlagsTest, PositionalArgsCollected) {
  ASSERT_TRUE(Parse({"prog", "--flag-a", "p1", "p2"}).ok());
  EXPECT_TRUE(cmd_.flag_a);
  EXPECT_EQ(ctx_.positional_args, (std::vector<std::string>{"p1", "p2"}));
}

TEST_F(ParseFlagsTest, FlagsAndPositionalsInterspersed) {
  ASSERT_TRUE(Parse({"prog", "p1", "--flag-a", "p2"}).ok());
  EXPECT_TRUE(cmd_.flag_a);
  EXPECT_EQ(ctx_.positional_args, (std::vector<std::string>{"p1", "p2"}));
}

TEST_F(ParseFlagsTest, OnlyPositionals) {
  ASSERT_TRUE(Parse({"prog", "x", "y"}).ok());
  EXPECT_EQ(ctx_.positional_args, (std::vector<std::string>{"x", "y"}));
}

TEST_F(ParseFlagsTest, GlobalFlagsMergedWithSubcommandFlags) {
  ASSERT_TRUE(Parse({"prog", "--dev", "--flag-a"}).ok());
  EXPECT_TRUE(global_.dev);
  EXPECT_TRUE(cmd_.flag_a);
}

TEST_F(ParseFlagsTest, GlobalShortFlagWithArg) {
  ASSERT_TRUE(Parse({"prog", "-m", "trace.mt"}).ok());
  EXPECT_EQ(global_.metatrace_path, "trace.mt");
}

TEST_F(ParseFlagsTest, UnknownFlagFailsAndMarksPrinted) {
  base::Status s = Parse({"prog", "--unknown-xyz"});
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.GetPayload("perfetto.dev/has_printed_error").has_value());
}

// --- FormatSubcommandUsage ---

TEST(FormatSubcommandUsageTest, ContainsAllSections) {
  FlagTestSubcommand cmd;
  std::string out = FormatSubcommandUsage("tp", &cmd);

  EXPECT_THAT(out, HasSubstr("Usage: tp fake [FLAGS] <args>"));
  EXPECT_THAT(out, HasSubstr("Fake description."));
  EXPECT_THAT(out, HasSubstr("Detailed help body."));
  EXPECT_THAT(out, HasSubstr("Subcommand flags:"));
  EXPECT_THAT(out, HasSubstr("--flag-a"));
  EXPECT_THAT(out, HasSubstr("--str"));
  EXPECT_THAT(out, HasSubstr("--rep"));
  EXPECT_THAT(out, HasSubstr("Global flags:"));
  EXPECT_THAT(out, HasSubstr("--help"));
}

// Regression test: the bundle subcommand used to list its flags both in a
// hand-written "Options:" block inside detailed_help() and again in the
// auto-generated "Subcommand flags:" section. Each flag must appear once.
TEST(FormatSubcommandUsageTest, BundleListsEachFlagExactlyOnce) {
  BundleSubcommand bundle;
  std::string out = FormatSubcommandUsage("tp", &bundle);

  EXPECT_EQ(CountOccurrences(out, "--symbol-paths"), 1u);
  EXPECT_EQ(CountOccurrences(out, "--no-auto-symbol-paths"), 1u);
  EXPECT_EQ(CountOccurrences(out, "--proguard-map"), 1u);
  EXPECT_EQ(CountOccurrences(out, "--no-auto-proguard-maps"), 1u);
  EXPECT_EQ(CountOccurrences(out, "--verbose"), 1u);
  EXPECT_THAT(out, Not(HasSubstr("Options:")));
}

// --- bundle argument validation ---

TEST(BundleSubcommandTest, RequiresInputAndOutput) {
  BundleSubcommand bundle;
  EXPECT_FALSE(bundle.Run(CtxWithPositionals({})).ok());
  EXPECT_FALSE(bundle.Run(CtxWithPositionals({"only_input"})).ok());
}

TEST(BundleSubcommandTest, RejectsStdinInput) {
  BundleSubcommand bundle;
  base::Status s = bundle.Run(CtxWithPositionals({"-", "out.tar"}));
  EXPECT_FALSE(s.ok());
  EXPECT_THAT(s.message(), HasSubstr("stdin"));
}

TEST(BundleSubcommandTest, RejectsStdoutOutput) {
  BundleSubcommand bundle;
  base::Status s = bundle.Run(CtxWithPositionals({"input.pftrace", "-"}));
  EXPECT_FALSE(s.ok());
  EXPECT_THAT(s.message(), HasSubstr("stdout"));
}

TEST(BundleSubcommandTest, RejectsMissingInputFile) {
  BundleSubcommand bundle;
  base::Status s =
      bundle.Run(CtxWithPositionals({"/no/such/input_xyz", "out.tar"}));
  EXPECT_FALSE(s.ok());
  EXPECT_THAT(s.message(), HasSubstr("does not exist"));
}

// --- convert argument validation ---

TEST(ConvertSubcommandTest, RequiresFormat) {
  ConvertSubcommand convert;
  base::Status s = convert.Run(CtxWithPositionals({}));
  EXPECT_FALSE(s.ok());
  EXPECT_THAT(s.message(), HasSubstr("format"));
}

TEST(ConvertSubcommandTest, RejectsUnknownFormat) {
  base::TempFile input = WriteTempFile("ignored");
  base::TempFile output = base::TempFile::Create();

  ConvertSubcommand convert;
  base::Status s =
      convert.Run(CtxWithPositionals({"bogus", input.path(), output.path()}));
  EXPECT_FALSE(s.ok());
  EXPECT_THAT(s.message(), HasSubstr("unknown format"));
}

// binary/decompress_packets moved to `util`, and java_heap_profile was removed
// in favour of `convert profile --java-heap`; `convert` must reject them all.
TEST(ConvertSubcommandTest, RejectsFormatsMovedOrRemoved) {
  base::TempFile input = WriteTempFile("ignored");
  base::TempFile output = base::TempFile::Create();

  for (const char* fmt :
       {"binary", "decompress_packets", "java_heap_profile"}) {
    ConvertSubcommand convert;
    base::Status s =
        convert.Run(CtxWithPositionals({fmt, input.path(), output.path()}));
    EXPECT_FALSE(s.ok()) << fmt;
    EXPECT_THAT(s.message(), HasSubstr("unknown format")) << fmt;
  }
}

TEST(ConvertSubcommandTest, RejectsInvalidTruncate) {
  ConvertSubcommand convert;
  GlobalOptions global;
  SubcommandContext ctx;
  ctx.global = &global;
  ASSERT_TRUE(
      RunParse(&convert, &ctx, {"prog", "--truncate", "sideways", "json"})
          .ok());
  base::Status s = convert.Run(ctx);
  EXPECT_FALSE(s.ok());
  EXPECT_THAT(s.message(), HasSubstr("truncate"));
}

TEST(ConvertSubcommandTest, RejectsPidForNonProfileFormat) {
  ConvertSubcommand convert;
  GlobalOptions global;
  SubcommandContext ctx;
  ctx.global = &global;
  ASSERT_TRUE(RunParse(&convert, &ctx, {"prog", "--pid", "5", "json"}).ok());
  base::Status s = convert.Run(ctx);
  EXPECT_FALSE(s.ok());
  EXPECT_THAT(s.message(), HasSubstr("profile"));
}

// --- util argument validation ---

TEST(UtilSubcommandTest, RequiresUtility) {
  UtilSubcommand util;
  base::Status s = util.Run(CtxWithPositionals({}));
  EXPECT_FALSE(s.ok());
  EXPECT_THAT(s.message(), HasSubstr("utility"));
}

TEST(UtilSubcommandTest, RejectsUnknownUtility) {
  UtilSubcommand util;
  base::Status s = util.Run(CtxWithPositionals({"bogus"}));
  EXPECT_FALSE(s.ok());
  EXPECT_THAT(s.message(), HasSubstr("unknown utility"));
}

// text_to_binary moved here from `convert binary`: a text-format trace proto
// must be converted to a non-empty binary trace.
TEST(UtilSubcommandTest, TextToBinaryConvertsTextProto) {
  base::TempFile input = WriteTempFile("packet { timestamp: 42 }");
  base::TempFile output = base::TempFile::Create();

  UtilSubcommand util;
  base::Status s = util.Run(
      CtxWithPositionals({"text_to_binary", input.path(), output.path()}));
  ASSERT_TRUE(s.ok()) << s.c_message();

  std::string out;
  ASSERT_TRUE(base::ReadFile(output.path(), &out));
  EXPECT_FALSE(out.empty());
}

}  // namespace
}  // namespace perfetto::trace_processor::shell
