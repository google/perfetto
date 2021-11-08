/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "perfetto/ext/base/getopt_compat.h"

// This test has two roles:
// 1. In Windows builds it's a plain unittest for our getopt_compat.cc
// 2. On other builds it also checks that the behavior of our getopt_compat.cc
//    is the same of <getopt.h> (for the options we support).
// It does so creating a gtest typed test, and defining two structs that inject
// getopt functions and global variables like optind.

#include "perfetto/base/build_config.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <getopt.h>
#endif

#include <initializer_list>

#include "test/gtest_and_gmock.h"

using testing::ElementsAre;
using testing::ElementsAreArray;

namespace perfetto {
namespace base {
namespace {

struct OurGetopt {
  using LongOptionType = getopt_compat::option;
  using GetoptFn = decltype(&getopt_compat::getopt);
  using GetoptLongFn = decltype(&getopt_compat::getopt_long);
  GetoptFn getopt = &getopt_compat::getopt;
  GetoptLongFn getopt_long = &getopt_compat::getopt_long;
  int& optind = getopt_compat::optind;
  int& optopt = getopt_compat::optopt;
  int& opterr = getopt_compat::opterr;
  char*& optarg = getopt_compat::optarg;
};

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
struct SystemGetopt {
  using LongOptionType = ::option;
  using GetoptFn = decltype(&::getopt);
  using GetoptLongFn = decltype(&::getopt_long);
  GetoptFn getopt = &::getopt;
  GetoptLongFn getopt_long = &::getopt_long;
  int& optind = ::optind;
  int& optopt = ::optopt;
  int& opterr = ::opterr;
  char*& optarg = ::optarg;
};
#endif

template <typename T>
class GetoptCompatTest : public testing::Test {
 public:
  inline void SetCmdline(std::initializer_list<const char*> arg_list) {
    // Reset the getopt() state.
    // When calling getopt() several times, MacOS requires that optind is reset
    // to 1, while Linux requires optind to be reset to 0. Also MacOS requires
    // optreset to be set as well.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
    impl.optind = 1;
    optreset = 1;  // It has no corresponding variable in other OSes.
#else
    impl.optind = 0;
#endif
    argc = static_cast<int>(arg_list.size());
    for (char*& arg : argv)
      arg = nullptr;
    size_t i = 0;
    for (const char* arg : arg_list)
      argv[i++] = const_cast<char*>(arg);
  }
  int argc;
  char* argv[32];  // We don't use more than 32 entries on our tests.
  T impl;
};

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
using GetoptTestTypes = ::testing::Types<OurGetopt>;
#else
using GetoptTestTypes = ::testing::Types<OurGetopt, SystemGetopt>;
#endif
TYPED_TEST_SUITE(GetoptCompatTest, GetoptTestTypes, /* trailing ',' for GCC*/);

TYPED_TEST(GetoptCompatTest, ShortOptions) {
  auto& t = this->impl;

  const char* sops = "";
  this->SetCmdline({"argv0"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);

  sops = "h";
  this->SetCmdline({"argv0"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);

  sops = "h";
  this->SetCmdline({"argv0", "-h"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'h');
  EXPECT_EQ(t.optind, 2);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 2);

  sops = "h";
  this->SetCmdline({"argv0", "positional1", "positional2"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);

  sops = "h";
  this->SetCmdline({"argv0", "--", "positional1", "positional2"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 2);

  sops = "h";
  this->SetCmdline({"argv0", "-h"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'h');
  EXPECT_EQ(t.optind, 2);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 2);

  sops = "abc";
  this->SetCmdline({"argv0", "-c", "-a", "-b"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'c');
  EXPECT_EQ(t.optind, 2);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'a');
  EXPECT_EQ(t.optind, 3);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'b');
  EXPECT_EQ(t.optind, 4);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 4);

  sops = "abc";
  this->SetCmdline({"argv0", "-c", "-a", "--", "nonopt"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'c');
  EXPECT_EQ(t.optind, 2);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'a');
  EXPECT_EQ(t.optind, 3);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 4);

  sops = "abc";
  this->SetCmdline({"argv0", "-cb"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'c');
  EXPECT_EQ(t.optind, 1);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'b');
  EXPECT_EQ(t.optind, 2);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 2);

  sops = "abc";
  this->SetCmdline({"argv0", "-aa", "-c"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'a');
  EXPECT_EQ(t.optind, 1);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'a');
  EXPECT_EQ(t.optind, 2);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'c');
  EXPECT_EQ(t.optind, 3);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 3);

  sops = "a:bc";
  // The semantic here is `-a b -c`
  this->SetCmdline({"argv0", "-ab", "-c"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'a');
  EXPECT_EQ(t.optind, 2);
  EXPECT_STREQ(t.optarg, "b");
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'c');
  EXPECT_EQ(t.optind, 3);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 3);

  sops = "a:bc";
  this->SetCmdline({"argv0", "-ab", "--", "-c"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'a');
  EXPECT_EQ(t.optind, 2);
  EXPECT_STREQ(t.optarg, "b");
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 3);

  sops = "a:b:c:";
  this->SetCmdline({"argv0", "-a", "arg1", "-b", "--", "-c", "-carg"});
  // This is sbutle, the "--" is an arg value for "-b", not a separator.
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'a');
  EXPECT_STREQ(t.optarg, "arg1");
  EXPECT_EQ(t.optind, 3);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'b');
  EXPECT_STREQ(t.optarg, "--");
  EXPECT_EQ(t.optind, 5);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'c');
  EXPECT_STREQ(t.optarg, "-carg");
  EXPECT_EQ(t.optind, 7);
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);
  EXPECT_EQ(t.optind, 7);

  sops = "a";
  this->SetCmdline({"argv0", "-q"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), '?');
  EXPECT_EQ(t.optind, 2);
}

TYPED_TEST(GetoptCompatTest, LongOptions) {
  auto& t = this->impl;
  using LongOptionType = typename decltype(this->impl)::LongOptionType;

  {
    LongOptionType lopts[]{
        {nullptr, 0, nullptr, 0},
    };
    const char* sops = "";
    this->SetCmdline({"argv0"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), -1);
    EXPECT_EQ(t.optind, 1);
  }

  {
    LongOptionType lopts[]{
        {nullptr, 0, nullptr, 0},
    };
    const char* sops = "";
    this->SetCmdline({"argv0", "--unknown"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), '?');
    EXPECT_EQ(t.optind, 2);
  }

  {
    LongOptionType lopts[]{
        {"one", 0 /*no_argument*/, nullptr, 1},
        {"two", 0 /*no_argument*/, nullptr, 2},
        {nullptr, 0, nullptr, 0},
    };
    const char* sops = "";
    this->SetCmdline({"argv0", "--two", "--one"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 2);
    EXPECT_EQ(t.optind, 2);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 1);
    EXPECT_EQ(t.optind, 3);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), -1);
    EXPECT_EQ(t.optind, 3);
  }

  {
    LongOptionType lopts[]{
        {"one", 0 /*no_argument*/, nullptr, 1},
        {"two", 0 /*no_argument*/, nullptr, 2},
        {nullptr, 0, nullptr, 0},
    };
    const char* sops = "";
    this->SetCmdline({"argv0", "--two", "--one", "--not-an-opt"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 2);
    EXPECT_EQ(t.optind, 2);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 1);
    EXPECT_EQ(t.optind, 3);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), '?');
    EXPECT_EQ(t.optind, 4);
  }

  {
    LongOptionType lopts[]{
        {"one", 0 /*no_argument*/, nullptr, 1},
        {"two", 0 /*no_argument*/, nullptr, 2},
        {nullptr, 0, nullptr, 0},
    };
    const char* sops = "";
    this->SetCmdline({"argv0", "--two", "--one", "--", "--not-an-opt"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 2);
    EXPECT_EQ(t.optind, 2);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 1);
    EXPECT_EQ(t.optind, 3);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), -1);
    EXPECT_EQ(t.optind, 4);
  }

  {
    LongOptionType lopts[]{
        {"no1", 0 /*no_argument*/, nullptr, 1},
        {"req2", 1 /*required_argument*/, nullptr, 2},
        {"req3", 1 /*required_argument*/, nullptr, 3},
        {nullptr, 0, nullptr, 0},
    };
    const char* sops = "";
    // This is subtle: the "--" really is an argument for req2, not an argument
    // separator. The first positional arg is "!!!".
    this->SetCmdline({"argv0", "--req3", "-", "--no1", "--req2", "--", "!!!"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 3);
    EXPECT_EQ(t.optind, 3);
    EXPECT_STREQ(t.optarg, "-");
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 1);
    EXPECT_EQ(t.optind, 4);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 2);
    EXPECT_STREQ(t.optarg, "--");
    EXPECT_EQ(t.optind, 6);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), -1);
    EXPECT_EQ(t.optind, 6);
  }

  {
    LongOptionType lopts[]{
        {"no1", 0 /*no_argument*/, nullptr, 1},
        {"req2", 1 /*required_argument*/, nullptr, 2},
        {nullptr, 0, nullptr, 0},
    };
    const char* sops = "";
    this->SetCmdline({"argv0", "--req2", "foo", "--", "--no1"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 2);
    EXPECT_EQ(t.optind, 3);
    EXPECT_STREQ(t.optarg, "foo");
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), -1);
    EXPECT_EQ(t.optind, 4);
  }
}

TYPED_TEST(GetoptCompatTest, ShortAndLongOptions) {
  auto& t = this->impl;
  using LongOptionType = typename decltype(this->impl)::LongOptionType;

  {
    LongOptionType lopts[]{
        {"one", 0 /*no_argument*/, nullptr, 1},
        {"two", 0 /*no_argument*/, nullptr, 2},
        {"three", 0 /*no_argument*/, nullptr, 3},
        {nullptr, 0, nullptr, 0},
    };
    const char* sops = "123";

    this->SetCmdline({"argv0"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), -1);
    EXPECT_EQ(t.optind, 1);

    this->SetCmdline({"argv0", "-13", "--two", "--three", "--", "--one"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), '1');
    EXPECT_EQ(t.optind, 1);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), '3');
    EXPECT_EQ(t.optind, 2);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 2);
    EXPECT_EQ(t.optind, 3);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 3);
    EXPECT_EQ(t.optind, 4);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), -1);
    EXPECT_EQ(t.optind, 5);

    this->SetCmdline({"argv0", "--two", "-1", "--two", "-13"});
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 2);
    EXPECT_EQ(t.optind, 2);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), '1');
    EXPECT_EQ(t.optind, 3);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 2);
    EXPECT_EQ(t.optind, 4);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), '1');
    EXPECT_EQ(t.optind, 4);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), '3');
    EXPECT_EQ(t.optind, 5);
    EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), -1);
    EXPECT_EQ(t.optind, 5);
  }
}

TYPED_TEST(GetoptCompatTest, OpterrHandling) {
  auto& t = this->impl;
  t.opterr = 0;  // Make errors silent.

  const char* sops = "ab:";
  this->SetCmdline({"argv0", "-a", "-c", "-b"});
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), 'a');
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), '?');
  EXPECT_EQ(t.optopt, 'c');
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), '?');
  EXPECT_EQ(t.optopt, 'b');
  EXPECT_EQ(t.getopt(this->argc, this->argv, sops), -1);

  using LongOptionType = typename decltype(this->impl)::LongOptionType;
  LongOptionType lopts[]{
      {"requires_arg", 1 /*required_argument*/, nullptr, 42},
      {nullptr, 0, nullptr, 0},
  };
  this->SetCmdline({"argv0", "-a", "--unkonwn", "--requires_arg"});
  EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), 'a');
  EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), '?');
  EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), '?');
  EXPECT_EQ(t.optopt, 42);
  EXPECT_EQ(t.getopt_long(this->argc, this->argv, sops, lopts, nullptr), -1);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
