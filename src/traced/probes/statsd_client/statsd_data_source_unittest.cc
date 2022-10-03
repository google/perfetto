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

#include "src/traced/probes/statsd_client/statsd_data_source.h"

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/statsd/statsd_atom.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#include "protos/perfetto/config/statsd/statsd_tracing_config.gen.h"
#include "protos/perfetto/trace/statsd/statsd_atom.gen.h"
#include "protos/third_party/statsd/shell_config.pbzero.h"

using ::perfetto::protos::gen::StatsdTracingConfig;
using ::perfetto::protos::pbzero::StatsdShellSubscription;
using ::perfetto::protos::pbzero::StatsdSimpleAtomMatcher;
using ::testing::Mock;

namespace perfetto {
namespace {

class TestStatsdDataSource : public StatsdDataSource {
 public:
  TestStatsdDataSource(base::TaskRunner* task_runner,
                       TracingSessionID id,
                       std::unique_ptr<TraceWriter> writer,
                       const DataSourceConfig& config)
      : StatsdDataSource(task_runner, id, std::move(writer), config) {}
};

class StatsdDataSourceTest : public ::testing::Test {
 protected:
  StatsdDataSourceTest() {}

  std::unique_ptr<TestStatsdDataSource> GetStatsdDataSource(
      const DataSourceConfig& cfg) {
    auto writer =
        std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
    writer_raw_ = writer.get();
    return std::unique_ptr<TestStatsdDataSource>(
        new TestStatsdDataSource(&task_runner_, 0, std::move(writer), cfg));
  }

  base::TestTaskRunner task_runner_;
  TraceWriterForTesting* writer_raw_;
};

TEST_F(StatsdDataSourceTest, EmptyTest) {}

TEST(StatsdDataSourceStaticTest, EmptyConfig) {
  DataSourceConfig cfg{};
  std::string s = StatsdDataSource::GenerateShellConfig(cfg);
  EXPECT_EQ(s, "");
}

TEST(StatsdDataSourceStaticTest, PushOneAtom) {
  StatsdTracingConfig cfg;
  cfg.add_raw_push_atom_id(42);

  DataSourceConfig ds_cfg;
  ds_cfg.set_statsd_tracing_config_raw(cfg.SerializeAsString());

  std::string s = StatsdDataSource::GenerateShellConfig(ds_cfg);
  StatsdShellSubscription::Decoder subscription(s);

  EXPECT_TRUE(subscription.has_pushed());
  EXPECT_EQ(StatsdSimpleAtomMatcher::Decoder(*subscription.pushed()).atom_id(),
            42);
}

}  // namespace
}  // namespace perfetto
