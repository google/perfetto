/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/traced/probes/ftrace/printk_formats_parser.h"

#include "test/gtest_and_gmock.h"

using ::testing::Contains;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Key;
using ::testing::Not;
using ::testing::Pair;

namespace perfetto {
namespace {

TEST(PrintkFormatParserTest, AllZeros) {
  std::string format = R"(0x0 : "Rescheduling interrupts"
0x0 : "Function call interrupts"
0x0 : "CPU stop interrupts"
0x0 : "Timer broadcast interrupts"
0x0 : "IRQ work interrupts"
0x0 : "CPU wakeup interrupts"
0x0 : "CPU backtrace"
0x0 : "rcu_sched"
0x0 : "rcu_bh"
0x0 : "rcu_preempt"
)";

  PrintkMap result = ParsePrintkFormats(format);
  EXPECT_THAT(result, IsEmpty());
}

TEST(PrintkFormatParserTest, VariousAddresses) {
  std::string format = R"(0x1 : "First line"
0x1 : "First line"
0x2 : "Unfortunate: colon"
0x3 : ""
0xffffff92349439b8 : "Large address"
0x9 : "Last line")";

  PrintkMap result = ParsePrintkFormats(format);
  EXPECT_THAT(result.at(1), Eq("First line"));
  EXPECT_THAT(result.at(2), Eq("Unfortunate: colon"));
  EXPECT_THAT(result.at(18446743602145278392ULL), Eq("Large address"));
  EXPECT_THAT(result.at(9), Eq("Last line"));
  EXPECT_THAT(result.at(3), Eq(""));
}

TEST(PrintkFormatParserTest, RobustToRubbish) {
  std::string format = R"(
: leading colon
trailing colon:
multiple colons: : : : :
Empty line:

Just colon:
:
: "No address"
No name:
0x1 :
0xbadhexaddress : "Bad hex address"
0x2 : No quotes
0x3:"No gap"
"Wrong way round" : 0x4
)";

  PrintkMap result = ParsePrintkFormats(format);
  EXPECT_THAT(result.at(2), Eq("No quotes"));
  EXPECT_THAT(result.at(3), Eq("No gap"));
}

}  // namespace
}  // namespace perfetto
