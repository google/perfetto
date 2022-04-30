/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/profiling/memory/heapprofd_producer.h"

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/commit_data_request.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "src/base/test/test_task_runner.h"
#include "src/tracing/test/mock_producer_endpoint.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {

using ::testing::Contains;
using ::testing::Eq;
using ::testing::Pair;
using ::testing::Property;

TEST(LogHistogramTest, Simple) {
  LogHistogram h;
  h.Add(1);
  h.Add(0);
  EXPECT_THAT(h.GetData(), Contains(Pair(2, 1)));
  EXPECT_THAT(h.GetData(), Contains(Pair(1, 1)));
}

TEST(LogHistogramTest, Overflow) {
  LogHistogram h;
  h.Add(std::numeric_limits<uint64_t>::max());
  EXPECT_THAT(h.GetData(), Contains(Pair(LogHistogram::kMaxBucket, 1)));
}

TEST(HeapprofdProducerTest, ExposesDataSource) {
  base::TestTaskRunner task_runner;
  HeapprofdProducer producer(HeapprofdMode::kCentral, &task_runner,
                             /* exit_when_done= */ false);

  std::unique_ptr<MockProducerEndpoint> endpoint(new MockProducerEndpoint());
  EXPECT_CALL(*endpoint,
              RegisterDataSource(Property(&DataSourceDescriptor::name,
                                          Eq("android.heapprofd"))))
      .Times(1);
  producer.SetProducerEndpoint(std::move(endpoint));
  producer.OnConnect();
}

TEST(HeapprofdConfigToClientConfigurationTest, Smoke) {
  HeapprofdConfig cfg;
  cfg.add_heaps("foo");
  cfg.set_sampling_interval_bytes(4096);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 1u);
  EXPECT_STREQ(cli_config.heaps[0].name, "foo");
  EXPECT_EQ(cli_config.heaps[0].interval, 4096u);
}

TEST(HeapprofdConfigToClientConfigurationTest, DefaultHeap) {
  HeapprofdConfig cfg;
  cfg.set_sampling_interval_bytes(4096);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 1u);
  EXPECT_STREQ(cli_config.heaps[0].name, "libc.malloc");
  EXPECT_EQ(cli_config.heaps[0].interval, 4096u);
}

TEST(HeapprofdConfigToClientConfigurationTest, TwoHeaps) {
  HeapprofdConfig cfg;
  cfg.add_heaps("foo");
  cfg.add_heaps("bar");
  cfg.set_sampling_interval_bytes(4096);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 2u);
  EXPECT_STREQ(cli_config.heaps[0].name, "foo");
  EXPECT_STREQ(cli_config.heaps[1].name, "bar");
  EXPECT_EQ(cli_config.heaps[0].interval, 4096u);
  EXPECT_EQ(cli_config.heaps[1].interval, 4096u);
}

TEST(HeapprofdConfigToClientConfigurationTest, TwoHeapsIntervals) {
  HeapprofdConfig cfg;
  cfg.add_heaps("foo");
  cfg.add_heap_sampling_intervals(4096u);
  cfg.add_heaps("bar");
  cfg.add_heap_sampling_intervals(1u);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 2u);
  EXPECT_STREQ(cli_config.heaps[0].name, "foo");
  EXPECT_STREQ(cli_config.heaps[1].name, "bar");
  EXPECT_EQ(cli_config.heaps[0].interval, 4096u);
  EXPECT_EQ(cli_config.heaps[1].interval, 1u);
}

TEST(HeapprofdConfigToClientConfigurationTest, OverflowHeapName) {
  std::string large_name(100, 'a');
  HeapprofdConfig cfg;
  cfg.add_heaps(large_name);
  cfg.set_sampling_interval_bytes(1);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 0u);
}

TEST(HeapprofdConfigToClientConfigurationTest, OverflowHeapNameAndValid) {
  std::string large_name(100, 'a');
  HeapprofdConfig cfg;
  cfg.add_heaps(large_name);
  cfg.add_heaps("foo");
  cfg.set_sampling_interval_bytes(1);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 1u);
  EXPECT_STREQ(cli_config.heaps[0].name, "foo");
}

TEST(HeapprofdConfigToClientConfigurationTest, ZeroSampling) {
  HeapprofdConfig cfg;
  cfg.add_heaps("foo");
  cfg.set_sampling_interval_bytes(0);
  ClientConfiguration cli_config;
  EXPECT_FALSE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
}

TEST(HeapprofdConfigToClientConfigurationTest, ZeroSamplingMultiple) {
  HeapprofdConfig cfg;
  cfg.add_heaps("foo");
  cfg.add_heap_sampling_intervals(4096u);
  cfg.add_heaps("bar");
  cfg.add_heap_sampling_intervals(0);
  ClientConfiguration cli_config;
  EXPECT_FALSE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
}

TEST(HeapprofdConfigToClientConfigurationTest, AdaptiveSampling) {
  HeapprofdConfig cfg;
  cfg.add_heaps("foo");
  cfg.set_sampling_interval_bytes(4096);
  cfg.set_adaptive_sampling_shmem_threshold(1024u);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 1u);
  EXPECT_STREQ(cli_config.heaps[0].name, "foo");
  EXPECT_EQ(cli_config.heaps[0].interval, 4096u);
  EXPECT_EQ(cli_config.adaptive_sampling_shmem_threshold, 1024u);
  EXPECT_EQ(cli_config.adaptive_sampling_max_sampling_interval_bytes, 0u);
}

TEST(HeapprofdConfigToClientConfigurationTest, AdaptiveSamplingWithMax) {
  HeapprofdConfig cfg;
  cfg.add_heaps("foo");
  cfg.set_sampling_interval_bytes(4096);
  cfg.set_adaptive_sampling_shmem_threshold(1024u);
  cfg.set_adaptive_sampling_max_sampling_interval_bytes(4 * 4096u);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 1u);
  EXPECT_STREQ(cli_config.heaps[0].name, "foo");
  EXPECT_EQ(cli_config.heaps[0].interval, 4096u);
  EXPECT_EQ(cli_config.adaptive_sampling_shmem_threshold, 1024u);
  EXPECT_EQ(cli_config.adaptive_sampling_max_sampling_interval_bytes,
            4 * 4096u);
}

TEST(HeapprofdConfigToClientConfigurationTest, AllHeaps) {
  HeapprofdConfig cfg;
  cfg.set_all_heaps(true);
  cfg.set_sampling_interval_bytes(4096);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 0u);
  EXPECT_EQ(cli_config.default_interval, 4096u);
}

TEST(HeapprofdConfigToClientConfigurationTest, AllHeapsAndExplicit) {
  HeapprofdConfig cfg;
  cfg.set_all_heaps(true);
  cfg.set_sampling_interval_bytes(4096);
  cfg.add_heaps("foo");
  cfg.add_heap_sampling_intervals(1024u);
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 1u);
  EXPECT_STREQ(cli_config.heaps[0].name, "foo");
  EXPECT_EQ(cli_config.heaps[0].interval, 1024u);
  EXPECT_EQ(cli_config.default_interval, 4096u);
}

TEST(HeapprofdConfigToClientConfigurationTest, AllHeapsAndDisabled) {
  HeapprofdConfig cfg;
  cfg.set_all_heaps(true);
  cfg.set_sampling_interval_bytes(4096);
  cfg.add_exclude_heaps("foo");
  ClientConfiguration cli_config;
  ASSERT_TRUE(HeapprofdConfigToClientConfiguration(cfg, &cli_config));
  EXPECT_EQ(cli_config.num_heaps, 1u);
  EXPECT_STREQ(cli_config.heaps[0].name, "foo");
  EXPECT_EQ(cli_config.heaps[0].interval, 0u);
  EXPECT_EQ(cli_config.default_interval, 4096u);
}

}  // namespace profiling
}  // namespace perfetto
