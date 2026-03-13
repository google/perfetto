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

#include "perfetto/ext/base/getopt.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::shell {
namespace {

// A minimal Subcommand implementation for testing.
class FakeSubcommand : public Subcommand {
 public:
  explicit FakeSubcommand(const char* n, const option* opts = nullptr)
      : name_(n), opts_(opts) {}
  const char* name() const override { return name_; }
  const char* description() const override { return ""; }
  int Run(const SubcommandContext&, int, char**) override { return 0; }
  void PrintUsage(const char*) override {}
  const option* GetLongOptions() const override {
    static const option kEmpty[] = {{nullptr, 0, nullptr, 0}};
    return opts_ ? opts_ : kEmpty;
  }

 private:
  const char* name_;
  const option* opts_;
};

// Helper to build an argv array from an initializer list. The returned
// vector owns the strings; the second vector contains the char* pointers.
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
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, subs);
  EXPECT_EQ(result.subcommand, nullptr);
}

TEST(FindSubcommandTest, OnlyFlagsReturnsNull) {
  FakeSubcommand query("query");
  std::vector<Subcommand*> subs = {&query};

  auto args = ArgvHolder::Make({"tp_shell", "-v", "--full-sort"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, subs);
  EXPECT_EQ(result.subcommand, nullptr);
}

TEST(FindSubcommandTest, UnknownPositionalReturnsNull) {
  FakeSubcommand query("query");
  std::vector<Subcommand*> subs = {&query};

  auto args = ArgvHolder::Make({"tp_shell", "trace.pb"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, subs);
  EXPECT_EQ(result.subcommand, nullptr);
}

TEST(FindSubcommandTest, KnownSubcommandReturnsPtr) {
  FakeSubcommand query("query");
  FakeSubcommand serve("serve");
  std::vector<Subcommand*> subs = {&query, &serve};

  auto args = ArgvHolder::Make({"tp_shell", "query", "-c", "SELECT 1"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, subs);
  EXPECT_EQ(result.subcommand, &query);
  EXPECT_EQ(result.argv_index, 1);
}

TEST(FindSubcommandTest, FlagWithArgSkipsValue) {
  // A "classic" subcommand that declares --dev-flag with required_argument.
  static const option classic_opts[] = {
      {"dev-flag", required_argument, nullptr, 1000},
      {nullptr, 0, nullptr, 0},
  };
  FakeSubcommand classic("classic", classic_opts);
  FakeSubcommand query("query");
  std::vector<Subcommand*> subs = {&query};
  std::vector<Subcommand*> all = {&query, &classic};

  // --dev-flag takes an argument "x=y", so "query" at index 3 should be found.
  auto args =
      ArgvHolder::Make({"tp_shell", "--dev-flag", "x=y", "query", "trace.pb"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, all);
  EXPECT_EQ(result.subcommand, &query);
  EXPECT_EQ(result.argv_index, 3);
}

TEST(FindSubcommandTest, SubcommandAfterFlags) {
  FakeSubcommand query("query");
  std::vector<Subcommand*> subs = {&query};

  auto args =
      ArgvHolder::Make({"tp_shell", "--dev", "query", "-c", "sql", "trace.pb"});
  auto result = FindSubcommandInArgs(args.argc(), args.argv(), subs, subs);
  EXPECT_EQ(result.subcommand, &query);
  EXPECT_EQ(result.argv_index, 2);
}

}  // namespace
}  // namespace perfetto::trace_processor::shell
