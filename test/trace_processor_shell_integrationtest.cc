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

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/temp_file.h"
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
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Property;
using testing::SizeIs;

const std::string_view kSimpleSystrace = R"(# tracer
surfaceflinger-598   (  598) [004] .... 10852.771242: tracing_mark_write: B|598|some event
surfaceflinger-598   (  598) [004] .... 10852.771245: tracing_mark_write: E|598
)";

std::string ShellPath() {
  return base::GetCurExecutableDir() + "/trace_processor_shell";
}

// Writes kSimpleSystrace to a temp file and returns the TempFile object.
base::TempFile WriteSimpleSystrace() {
  auto f = base::TempFile::Create();
  base::WriteAll(f.fd(), kSimpleSystrace.data(),
                 static_cast<size_t>(kSimpleSystrace.size()));
  return f;
}

// Writes arbitrary content to a temp file and returns the TempFile object.
base::TempFile WriteTempFile(const std::string& content) {
  auto f = base::TempFile::Create();
  base::WriteAll(f.fd(), content.data(), content.size());
  return f;
}

struct SubprocessResult {
  int exit_code;
  std::string out;
};

// Runs trace_processor_shell with the given args. stdout and stderr are both
// captured into `out`.
SubprocessResult RunShell(std::initializer_list<std::string> extra_args) {
  base::Subprocess p;
  p.args.exec_cmd.push_back(ShellPath());
  for (const auto& a : extra_args) {
    p.args.exec_cmd.push_back(a);
  }
  p.args.stdin_mode = base::Subprocess::InputMode::kDevNull;
  p.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  p.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
  p.Start();
  PERFETTO_CHECK(p.Wait(kDefaultTestTimeoutMs));
  return {p.returncode(), std::move(p.output())};
}

// ---------------------------------------------------------------------------
// Classic CLI backcompat tests
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, ClassicVersion) {
  auto result = RunShell({"-v"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("Trace Processor RPC API version"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicHelp) {
  auto result = RunShell({"-h"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("Interactive trace processor shell"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicUnknownFlag) {
  auto result = RunShell({"--nonexistent"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicQueryString) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"-Q", "SELECT 1 AS x", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("x"));
  EXPECT_THAT(result.out, HasSubstr("1"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicQueryFile) {
  auto trace = WriteSimpleSystrace();
  auto query = WriteTempFile("SELECT 42 AS val;");
  auto result = RunShell({"-q", query.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("val"));
  EXPECT_THAT(result.out, HasSubstr("42"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicQueryStringNoTrace) {
  auto result = RunShell({"-Q", "SELECT 1"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicQueryFileBadPath) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"-q", "/nonexistent.sql", trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicFullSort) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"--full-sort", "-Q", "SELECT 1", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicNoFtraceRaw) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"--no-ftrace-raw", "-Q", "SELECT 1", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicCropTrackEvents) {
  auto trace = WriteSimpleSystrace();
  auto result =
      RunShell({"--crop-track-events", "-Q", "SELECT 1", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicAnalyzeProtoContent) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell(
      {"--analyze-trace-proto-content", "-Q", "SELECT 1", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicExport) {
  auto trace = WriteSimpleSystrace();
  auto out_db = base::TempFile::Create();
  auto result = RunShell({"-e", out_db.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(base::FileExists(out_db.path()));
}

TEST(TraceProcessorShellIntegrationTest, ClassicSummary) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"--summary", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicDev) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"--dev", "-Q", "SELECT 1", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicExtraChecks) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"--extra-checks", "-Q", "SELECT 1", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicSummaryAndMetricsConflict) {
  auto trace = WriteSimpleSystrace();
  auto result =
      RunShell({"--summary", "--run-metrics", "android_cpu", trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicWide) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"-W", "-Q", "SELECT 1", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicPerfFile) {
  auto trace = WriteSimpleSystrace();
  auto perf = base::TempFile::Create();
  auto result = RunShell({"-p", perf.path(), "-Q", "SELECT 1", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicPerfFileWithoutQueryFails) {
  auto trace = WriteSimpleSystrace();
  auto perf = base::TempFile::Create();
  // -p without -q/-Q means interactive mode, which rejects -p.
  auto result = RunShell({"-p", perf.path(), trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicExportWithQuery) {
  auto trace = WriteSimpleSystrace();
  auto out_db = base::TempFile::Create();
  auto query = WriteTempFile("SELECT 1;");
  auto result =
      RunShell({"-e", out_db.path(), "-q", query.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(base::FileExists(out_db.path()));
}

TEST(TraceProcessorShellIntegrationTest, ClassicSummaryWithQuery) {
  // --summary + -q: summary output suppressed, query runs.
  auto trace = WriteSimpleSystrace();
  auto query = WriteTempFile("SELECT 42 AS val;");
  auto result = RunShell({"--summary", "-q", query.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("42"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicInteractiveWithQuery) {
  // -i + -Q: runs query then drops into REPL (which exits on /dev/null stdin).
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"-i", "-Q", "SELECT 1 AS x", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("x"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicDefaultInteractive) {
  // No flags other than trace = interactive mode (exits on /dev/null stdin).
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicOverrideStdlibWithoutDevFails) {
  auto trace = WriteSimpleSystrace();
  auto result =
      RunShell({"--override-stdlib", "/tmp", "-Q", "SELECT 1", trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicStdiodNoTrace) {
  // --stdiod without trace file should work.
  TraceProcessorRpcStream req;
  auto* rpc = req.add_msg();
  rpc->set_request(TraceProcessorRpc::TPM_QUERY_STREAMING);
  rpc->mutable_query_args()->set_sql_query("SELECT 1 AS x");

  base::Subprocess process({ShellPath(), "--stdiod"});
  process.args.stdin_mode = base::Subprocess::InputMode::kBuffer;
  process.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.input = req.SerializeAsString();
  process.Start();

  ASSERT_TRUE(process.Wait(kDefaultTestTimeoutMs));

  TraceProcessorRpcStream stream;
  stream.ParseFromString(process.output());
  ASSERT_THAT(stream.msg(), SizeIs(1));
  ASSERT_EQ(stream.msg()[0].response(), TraceProcessorRpc::TPM_QUERY_STREAMING);
}

// ---------------------------------------------------------------------------
// Subcommand: query
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, QuerySubcommandSqlString) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"query", "-c", "SELECT 1 AS x", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("x"));
  EXPECT_THAT(result.out, HasSubstr("1"));
}

TEST(TraceProcessorShellIntegrationTest, QuerySubcommandSqlFile) {
  auto trace = WriteSimpleSystrace();
  auto query = WriteTempFile("SELECT 42 AS val;");
  auto result = RunShell({"query", "-f", query.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("val"));
  EXPECT_THAT(result.out, HasSubstr("42"));
}

TEST(TraceProcessorShellIntegrationTest, QuerySubcommandGlobalFlagBefore) {
  auto trace = WriteSimpleSystrace();
  auto result =
      RunShell({"--dev", "query", "-c", "SELECT 1 AS x", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("1"));
}

TEST(TraceProcessorShellIntegrationTest, QuerySubcommandNoQueryFails) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"query", trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, QuerySubcommandNoTraceFails) {
  auto result = RunShell({"query", "-c", "SELECT 1"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, QuerySubcommandBadFileFails) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"query", "-f", "/nonexistent.sql", trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

// ---------------------------------------------------------------------------
// Existing RPC test
// ---------------------------------------------------------------------------

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
