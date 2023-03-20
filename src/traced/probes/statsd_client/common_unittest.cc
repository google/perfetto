/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/traced/probes/statsd_client/common.h"

#include "perfetto/tracing/core/data_source_config.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/config/statsd/statsd_tracing_config.gen.h"
#include "protos/third_party/statsd/shell_config.pbzero.h"

using ::perfetto::protos::gen::StatsdTracingConfig;
using ::perfetto::protos::pbzero::StatsdShellSubscription;
using ::perfetto::protos::pbzero::StatsdSimpleAtomMatcher;

namespace perfetto {
namespace {

TEST(StatsdDataSourceCommonTest, EmptyConfig) {
  DataSourceConfig cfg{};
  std::string s = CreateStatsdShellConfig(cfg);
  EXPECT_EQ(s, "");
}

TEST(StatsdDataSourceCommonTest, PushOneAtom) {
  StatsdTracingConfig cfg;
  cfg.add_raw_push_atom_id(42);

  DataSourceConfig ds_cfg;
  ds_cfg.set_statsd_tracing_config_raw(cfg.SerializeAsString());

  std::string s = CreateStatsdShellConfig(ds_cfg);
  StatsdShellSubscription::Decoder subscription(s);

  EXPECT_TRUE(subscription.has_pushed());
  EXPECT_EQ(StatsdSimpleAtomMatcher::Decoder(*subscription.pushed()).atom_id(),
            42);
}

}  // namespace
}  // namespace perfetto
