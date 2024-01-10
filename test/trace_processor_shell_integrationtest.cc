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

#include <string_view>

#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/utils.h"
#include "protos/perfetto/trace_processor/trace_processor.gen.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

namespace perfetto::trace_processor {
namespace {

using TraceProcessorRpc = protos::gen::TraceProcessorRpc;
using TraceProcessorRpcStream = protos::gen::TraceProcessorRpcStream;
using CellsBatch = protos::gen::QueryResult::CellsBatch;

using testing::AllOf;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::Property;
using testing::SizeIs;

const std::string_view kSimpleSystrace = R"(# tracer
surfaceflinger-598   (  598) [004] .... 10852.771242: tracing_mark_write: B|598|some event
surfaceflinger-598   (  598) [004] .... 10852.771245: tracing_mark_write: E|598
)";

TEST(TraceProcessorShellIntegrationTest, StdioSimpleRequestResponse) {
  TraceProcessorRpcStream req;

  auto* rpc = req.add_msg();
  rpc->set_append_trace_data(kSimpleSystrace.data(), kSimpleSystrace.size());
  rpc->set_request(TraceProcessorRpc::TPM_APPEND_TRACE_DATA);

  rpc = req.add_msg();
  rpc->set_request(TraceProcessorRpc::TPM_FINALIZE_TRACE_DATA);

  rpc = req.add_msg();
  rpc->set_request(TraceProcessorRpc::TPM_QUERY_STREAMING);
  rpc->mutable_query_args()->set_sql_query("SELECT ts, dur FROM slice");

  base::Subprocess process(
      {base::GetCurExecutableDir() + "/trace_processor_shell", "--stdiod"});
  process.args.stdin_mode = base::Subprocess::InputMode::kBuffer;
  process.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.stderr_mode = base::Subprocess::OutputMode::kInherit;
  process.args.input = req.SerializeAsString();
  process.Start();

  ASSERT_TRUE(process.Wait(kDefaultTestTimeoutMs));

  TraceProcessorRpcStream stream;
  stream.ParseFromString(process.output());

  ASSERT_THAT(stream.msg(),
              ElementsAre(Property(&TraceProcessorRpc::response,
                                   TraceProcessorRpc::TPM_APPEND_TRACE_DATA),
                          Property(&TraceProcessorRpc::response,
                                   TraceProcessorRpc::TPM_FINALIZE_TRACE_DATA),
                          Property(&TraceProcessorRpc::response,
                                   TraceProcessorRpc::TPM_QUERY_STREAMING)));
  ASSERT_THAT(stream.msg()[0].append_result().error(), IsEmpty());
  ASSERT_THAT(stream.msg()[2].query_result().batch(), SizeIs(1));
  ASSERT_THAT(stream.msg()[2].query_result().batch()[0].varint_cells(),
              ElementsAre(10852771242000, 3000));
}

}  // namespace
}  // namespace perfetto::trace_processor
