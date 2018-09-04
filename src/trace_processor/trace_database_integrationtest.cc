/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <map>
#include <random>
#include <string>

#include "gtest/gtest.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "src/base/test/utils.h"
#include "src/trace_processor/json_trace_parser.h"
#include "src/trace_processor/trace_processor.h"

#include "perfetto/trace_processor/raw_query.pb.h"

namespace perfetto {
namespace trace_processor {
namespace {

class TraceProcessorIntegrationTest : public ::testing::Test {
 public:
  TraceProcessor processor;

 protected:
  bool LoadTrace(const char* name, int min_chunk_size = 1) {
    base::ScopedFstream f(fopen(base::GetTestDataPath(name).c_str(), "rb"));
    std::minstd_rand0 rnd_engine(0);
    std::uniform_int_distribution<> dist(min_chunk_size, 1024);
    while (!feof(*f)) {
      size_t chunk_size = static_cast<size_t>(dist(rnd_engine));
      std::unique_ptr<uint8_t[]> buf(new uint8_t[chunk_size]);
      auto rsize = fread(reinterpret_cast<char*>(buf.get()), 1, chunk_size, *f);
      if (!processor.Parse(std::move(buf), rsize))
        return false;
    }
    processor.NotifyEndOfFile();
    return true;
  }

  void Query(const std::string& query, protos::RawQueryResult* result) {
    protos::RawQueryArgs args;
    args.set_sql_query(query);
    auto on_result = [&result](const protos::RawQueryResult& res) {
      result->CopyFrom(res);
    };
    processor.ExecuteQuery(args, on_result);
  }
};

TEST_F(TraceProcessorIntegrationTest, AndroidSchedAndPs) {
  ASSERT_TRUE(LoadTrace("android_sched_and_ps.pb"));
  protos::RawQueryResult res;
  Query("select count(*), max(ts) - min(ts) from sched", &res);
  ASSERT_EQ(res.num_records(), 1);
  ASSERT_EQ(res.columns(0).long_values(0), 139789);
  ASSERT_EQ(res.columns(1).long_values(0), 19684308497);
}

TEST_F(TraceProcessorIntegrationTest, Sfgate) {
  ASSERT_TRUE(LoadTrace("sfgate.json", strlen(JsonTraceParser::kPreamble)));
  protos::RawQueryResult res;
  Query("select count(*), max(ts) - min(ts) from slices", &res);
  ASSERT_EQ(res.num_records(), 1);
  ASSERT_EQ(res.columns(0).long_values(0), 39830);
  ASSERT_EQ(res.columns(1).long_values(0), 40532506000);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
