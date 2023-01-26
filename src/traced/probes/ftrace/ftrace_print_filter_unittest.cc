/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/traced/probes/ftrace/ftrace_print_filter.h"

#include "protos/perfetto/config/ftrace/ftrace_config.gen.h"

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using perfetto::protos::gen::FtraceConfig;

TEST(FtracePrintFilterTest, EmptyConfigDefaultAllows) {
  FtraceConfig::PrintFilter conf;
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("word", 4));
}

TEST(FtracePrintFilterTest, OneRuleMatchesAllows) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  rule->set_prefix("w");
  rule->set_allow(true);
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("word", 4));
}

TEST(FtracePrintFilterTest, OneRuleMatchesDenies) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  rule->set_prefix("w");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_FALSE(filter.IsAllowed("word", 4));
}

TEST(FtracePrintFilterTest, OneRuleMatchesLongSize) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  rule->set_prefix("w");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_FALSE(filter.IsAllowed("word", 120));
}

TEST(FtracePrintFilterTest, OneRuleMatchesShortSize) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  rule->set_prefix("w");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_FALSE(filter.IsAllowed("word", 1));
}

TEST(FtracePrintFilterTest, OneRuleDoesntMatchLongSize) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  rule->set_prefix("verylongprefix");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("short", 120));
}

TEST(FtracePrintFilterTest, OneRuleWildcard) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  rule->set_prefix("");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_FALSE(filter.IsAllowed("anything", 8));
}

TEST(FtracePrintFilterTest, TwoRulesMatchFirst) {
  FtraceConfig::PrintFilter conf;
  {
    auto* rule = conf.add_rules();
    rule->set_prefix("word");
    rule->set_allow(false);
  }
  {
    auto* rule = conf.add_rules();
    rule->set_prefix("doesntmatch");
    rule->set_allow(true);
  }
  FtracePrintFilter filter(conf);

  EXPECT_FALSE(filter.IsAllowed("word", 120));
}

TEST(FtracePrintFilterTest, TwoRulesMatchesSecond) {
  FtraceConfig::PrintFilter conf;
  {
    auto* rule = conf.add_rules();
    rule->set_prefix("doesntmatch");
    rule->set_allow(true);
  }
  {
    auto* rule = conf.add_rules();
    rule->set_prefix("word");
    rule->set_allow(false);
  }
  FtracePrintFilter filter(conf);

  EXPECT_FALSE(filter.IsAllowed("word", 120));
}

TEST(FtracePrintFilterTest, AtraceRuleTypeDoesntMatch) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  auto* atrace = rule->mutable_atrace_msg();
  atrace->set_type("C");
  atrace->set_type("mycounter");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("B", 1));
}

TEST(FtracePrintFilterTest, AtraceRuleNoFirstSlash) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  auto* atrace = rule->mutable_atrace_msg();
  atrace->set_type("C");
  atrace->set_prefix("mycounter");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("Cnopipemycounter", 16));
}

TEST(FtracePrintFilterTest, AtraceRuleNoFirstSlashEnd) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  auto* atrace = rule->mutable_atrace_msg();
  atrace->set_type("C");
  atrace->set_prefix("mycounter");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("C", 1));
}

TEST(FtracePrintFilterTest, AtraceRuleNonIntPid) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  auto* atrace = rule->mutable_atrace_msg();
  atrace->set_type("C");
  atrace->set_prefix("mycounter");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("C|badpid|mycounter", 18));
}

TEST(FtracePrintFilterTest, AtraceRuleEndAfterPid) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  auto* atrace = rule->mutable_atrace_msg();
  atrace->set_type("C");
  atrace->set_prefix("mycounter");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("C|111111", 8));
}

TEST(FtracePrintFilterTest, AtraceRuleNoSecondSlash) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  auto* atrace = rule->mutable_atrace_msg();
  atrace->set_type("C");
  atrace->set_prefix("mycounter");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("C|111111Xmycounter", 18));
}

TEST(FtracePrintFilterTest, AtraceRuleAfterPrefixDoesntMatch) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  auto* atrace = rule->mutable_atrace_msg();
  atrace->set_type("C");
  atrace->set_prefix("mycounter");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_TRUE(filter.IsAllowed("C|111111|nomatch", 16));
}

TEST(FtracePrintFilterTest, AtraceRuleMatches) {
  FtraceConfig::PrintFilter conf;
  auto* rule = conf.add_rules();
  auto* atrace = rule->mutable_atrace_msg();
  atrace->set_type("C");
  atrace->set_prefix("mycounter");
  rule->set_allow(false);
  FtracePrintFilter filter(conf);

  EXPECT_FALSE(filter.IsAllowed("C|111111|mycounter...", 21));
}

}  // namespace
}  // namespace perfetto
