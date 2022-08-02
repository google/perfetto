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

#include "src/traced/probes/android_game_intervention_list/android_game_intervention_list_data_source.h"

#include <stdio.h>

#include <string>
#include <vector>

#include "perfetto/ext/base/pipe.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/data_source_config.h"

#include "protos/perfetto/config/android/android_game_intervention_list_config.gen.h"
#include "protos/perfetto/trace/android/android_game_intervention_list.gen.h"
#include "protos/perfetto/trace/android/android_game_intervention_list.pbzero.h"

#include "src/tracing/core/trace_writer_for_testing.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

class TestAndroidGameInterventionDataSource
    : public AndroidGameInterventionListDataSource {
 public:
  TestAndroidGameInterventionDataSource(
      const DataSourceConfig& ds_config,
      TracingSessionID session_id,
      std::unique_ptr<TraceWriter> trace_writer)
      : AndroidGameInterventionListDataSource(ds_config,
                                              session_id,
                                              std::move(trace_writer)) {}
};

class AndroidGameInterventionListDataSourceTest : public ::testing::Test {
 protected:
  AndroidGameInterventionListDataSourceTest() {}

  void CreateInstance(const DataSourceConfig& config) {
    auto writer =
        std::unique_ptr<TraceWriterForTesting>(new TraceWriterForTesting());
    trace_writer_ = writer.get();
    data_source_.reset(new TestAndroidGameInterventionDataSource(
        config, /* id */ 0, std::move(writer)));
  }

  std::unique_ptr<TestAndroidGameInterventionDataSource> data_source_;
  TraceWriterForTesting* trace_writer_;
};

TEST_F(AndroidGameInterventionListDataSourceTest, NonEmptyNameFilter) {
  static constexpr char kValidInterventionLines[] =
      "com.test.one\t1234\t0\t"
      "1\tangle=1,scaling=1.0,fps=0\t"
      "2\tangle=0,scaling=1.0,fps=60\n"
      "com.test.two\t1235\t1\t"
      "1\tangle=0,scaling=1.0,fps=0\t"
      "3\tangle=1,scaling=0.6,fps=45\n"
      "com.test.three\t1236\t2\t"
      "1\tangle=1,scaling=1.0,fps=0\t"
      "3\tangle=1,scaling=0.85,fps=30\t"
      "2\tangle=0,scaling=0.75,fps=120\n";

  CreateInstance(DataSourceConfig());

  auto pipe = base::Pipe::Create();
  PERFETTO_CHECK(write(pipe.wr.get(), kValidInterventionLines,
                       sizeof(kValidInterventionLines) - 1) ==
                 sizeof(kValidInterventionLines) - 1);
  pipe.wr.reset();
  auto file_stream = base::ScopedFstream(fdopen(pipe.rd.get(), "r"));
  pipe.rd.release();

  protozero::HeapBuffered<protos::pbzero::AndroidGameInterventionList>
      android_game_intervention_list;
  std::vector<std::string> name_filter = {"com.test.one", "com.test.three"};

  ASSERT_TRUE(data_source_->ParseAndroidGameInterventionListStream(
      android_game_intervention_list.get(), file_stream, name_filter));
  protos::gen::AndroidGameInterventionList parsed;
  parsed.ParseFromString(android_game_intervention_list.SerializeAsString());

  EXPECT_FALSE(parsed.read_error());
  EXPECT_FALSE(parsed.parse_error());

  EXPECT_EQ(parsed.game_packages_size(), 2);
  EXPECT_EQ(parsed.game_packages()[0].name(), "com.test.one");
  EXPECT_EQ(parsed.game_packages()[0].uid(), 1234ul);
  EXPECT_EQ(parsed.game_packages()[0].current_mode(), 0u);
  EXPECT_EQ(parsed.game_packages()[0].game_mode_info_size(), 2);
  EXPECT_EQ(parsed.game_packages()[0].game_mode_info()[0].mode(), 1u);
  EXPECT_EQ(parsed.game_packages()[0].game_mode_info()[0].use_angle(), true);
  EXPECT_EQ(
      parsed.game_packages()[0].game_mode_info()[0].resolution_downscale(),
      1.0f);
  EXPECT_EQ(parsed.game_packages()[0].game_mode_info()[0].fps(), 0.0f);
  EXPECT_EQ(parsed.game_packages()[0].game_mode_info()[1].mode(), 2u);
  EXPECT_EQ(parsed.game_packages()[0].game_mode_info()[1].use_angle(), false);
  EXPECT_EQ(
      parsed.game_packages()[0].game_mode_info()[1].resolution_downscale(),
      1.0f);
  EXPECT_EQ(parsed.game_packages()[0].game_mode_info()[1].fps(), 60.0f);

  EXPECT_EQ(parsed.game_packages()[1].name(), "com.test.three");
  EXPECT_EQ(parsed.game_packages()[1].uid(), 1236ul);
  EXPECT_EQ(parsed.game_packages()[1].current_mode(), 2u);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info_size(), 3);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info()[0].mode(), 1u);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info()[0].use_angle(), true);
  EXPECT_EQ(
      parsed.game_packages()[1].game_mode_info()[0].resolution_downscale(),
      1.0f);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info()[0].fps(), 0.0f);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info()[1].mode(), 3u);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info()[1].use_angle(), true);
  EXPECT_EQ(
      parsed.game_packages()[1].game_mode_info()[1].resolution_downscale(),
      0.85f);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info()[1].fps(), 30.0f);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info()[2].mode(), 2u);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info()[2].use_angle(), false);
  EXPECT_EQ(
      parsed.game_packages()[1].game_mode_info()[2].resolution_downscale(),
      0.75f);
  EXPECT_EQ(parsed.game_packages()[1].game_mode_info()[2].fps(), 120.0f);
}

}  // namespace
}  // namespace perfetto
