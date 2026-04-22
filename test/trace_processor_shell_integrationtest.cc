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

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "protos/perfetto/trace_processor/trace_processor.gen.h"
#include "src/base/test/utils.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <unistd.h>
#include <climits>
#endif

namespace perfetto::trace_processor {
namespace {

using TraceProcessorRpc = protos::gen::TraceProcessorRpc;
using TraceProcessorRpcStream = protos::gen::TraceProcessorRpcStream;
using CellsBatch = protos::gen::QueryResult::CellsBatch;

using testing::AllOf;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;
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
  // --help now shows the subcommand-aware help.
  auto result = RunShell({"-h"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("Perfetto Trace Processor"));
  EXPECT_THAT(result.out, HasSubstr("Commands:"));
  EXPECT_THAT(result.out, HasSubstr("query"));
  EXPECT_THAT(result.out, HasSubstr("interactive"));
  EXPECT_THAT(result.out, HasSubstr("server"));
  EXPECT_THAT(result.out, HasSubstr("summarize"));
  EXPECT_THAT(result.out, HasSubstr("metrics"));
  EXPECT_THAT(result.out, HasSubstr("export"));
}

TEST(TraceProcessorShellIntegrationTest, HelpClassic) {
  auto result = RunShell({"--help-classic"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("Interactive trace processor shell"));
}

TEST(TraceProcessorShellIntegrationTest, HelpCommand) {
  auto result = RunShell({"help", "query"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("run a SQL query"));
}

TEST(TraceProcessorShellIntegrationTest, HelpBare) {
  auto result = RunShell({"help"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("Commands:"));
}

TEST(TraceProcessorShellIntegrationTest, HelpUnknownCommand) {
  auto result = RunShell({"help", "nonexistent"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, NoFileCollisionHintOnNormalUsage) {
  // Normal subcommand usage should not emit the file collision hint.
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"query", trace.path(), "SELECT 1"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, Not(HasSubstr("matches both a subcommand")));
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
TEST(TraceProcessorShellIntegrationTest, FileCollisionHint) {
  // Create a file named "query" in a temp dir, chdir there, and run the shell
  // with bare "query" as an arg. The dispatcher should match "query" as a
  // subcommand but also notice the file and emit a hint.
  auto tmpdir = base::TempDir::Create();
  std::string file_path = std::string(tmpdir.path()) + "/query";
  base::WriteAll(*base::OpenFile(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0600),
                 kSimpleSystrace.data(),
                 static_cast<size_t>(kSimpleSystrace.size()));

  // RAII guard: restore CWD and clean up the file no matter how the test exits.
  char old_cwd[PATH_MAX];
  ASSERT_NE(getcwd(old_cwd, sizeof(old_cwd)), nullptr);
  ASSERT_EQ(chdir(tmpdir.path().c_str()), 0);
  auto cleanup = base::OnScopeExit([&] {
    PERFETTO_CHECK(chdir(old_cwd) == 0);
    remove(file_path.c_str());
  });

  // "query" is interpreted as the subcommand (which fails — no -f/-c), but
  // the hint should appear because a file named "query" exists in CWD.
  auto result = RunShell({"query"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("matches both a subcommand and a file"));
}
#endif

TEST(TraceProcessorShellIntegrationTest, ClassicUnknownFlag) {
  auto result = RunShell({"--nonexistent"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicQueryString) {
  // Use computed value: 200 + 61 = 261, "261" not in input SQL.
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"-Q", "SELECT 200 + 61 AS test_col", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("261"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicQueryFile) {
  // Use computed value: 400 + 76 = 476, "476" not in input SQL.
  auto trace = WriteSimpleSystrace();
  auto query = WriteTempFile("SELECT 400 + 76 AS unique_val;");
  auto result = RunShell({"-q", query.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("476"));
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

// ---------------------------------------------------------------------------
// Classic-to-subcommand translation tests
// These verify that classic flags are correctly translated to subcommand
// invocations by the translation layer.
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, ClassicBareTraceIsInteractive) {
  // Bare trace file with no flags -> interactive. Prove it entered the shell
  // by piping a computed query whose result (123) differs from the input SQL.
  auto trace = WriteSimpleSystrace();
  base::Subprocess p;
  p.args.exec_cmd = {ShellPath(), trace.path()};
  p.args.stdin_mode = base::Subprocess::InputMode::kBuffer;
  p.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  p.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
  p.args.input = "SELECT 100 + 23 AS computed;\n";
  p.Start();
  ASSERT_TRUE(p.Wait(kDefaultTestTimeoutMs));
  EXPECT_EQ(p.returncode(), 0);
  // "123" only appears in the computed result, not in the input "100 + 23".
  EXPECT_THAT(p.output(), HasSubstr("123"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicQueryFileInteractive) {
  // -q file -i -> query -f file -i (interactive after query).
  // The query creates a table; the interactive shell queries it with a computed
  // expression whose result (579) differs from the input.
  auto trace = WriteSimpleSystrace();
  auto sql =
      WriteTempFile("CREATE PERFETTO TABLE __test AS SELECT 500 AS val;");

  base::Subprocess p;
  p.args.exec_cmd = {ShellPath(), "-q", sql.path(), "-i", trace.path()};
  p.args.stdin_mode = base::Subprocess::InputMode::kBuffer;
  p.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  p.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
  p.args.input = "SELECT val + 79 AS result FROM __test;\n";
  p.Start();
  ASSERT_TRUE(p.Wait(kDefaultTestTimeoutMs));
  EXPECT_EQ(p.returncode(), 0);
  // "579" only appears in the computed result, not in the SQL or table data.
  EXPECT_THAT(p.output(), HasSubstr("579"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicQueryStringWide) {
  // -Q "SQL" -W -> query -W trace "SQL". Use a computed value (357) that
  // doesn't appear in the input SQL (300 + 57).
  auto trace = WriteSimpleSystrace();
  auto result =
      RunShell({"-W", "-Q", "SELECT 300 + 57 AS wide_col", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("357"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicStdiod) {
  // --stdiod -> server stdio
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

TEST(TraceProcessorShellIntegrationTest, ClassicStdiodWithTrace) {
  // --stdiod trace -> server stdio trace
  // Verify the translation works: process starts and exits cleanly.
  auto trace = WriteSimpleSystrace();
  base::Subprocess process({ShellPath(), "--stdiod", trace.path()});
  process.args.stdin_mode = base::Subprocess::InputMode::kBuffer;
  process.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.input = "";  // Empty stdin -> server exits.
  process.Start();
  ASSERT_TRUE(process.Wait(kDefaultTestTimeoutMs));
  EXPECT_EQ(process.returncode(), 0);
}

TEST(TraceProcessorShellIntegrationTest, ClassicRunMetrics) {
  // --run-metrics -> metrics --run. The metric output is a textproto
  // containing the metric name as a field.
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"--run-metrics", "android_cpu", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  // The output is a textproto with "android_cpu {" as a top-level field.
  EXPECT_THAT(result.out, HasSubstr("android_cpu {"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicExportQueryDisallowed) {
  // -e + -q should be disallowed with a clear error message.
  auto trace = WriteSimpleSystrace();
  auto query = WriteTempFile("SELECT 1;");
  auto out_db = base::TempFile::Create();
  auto result =
      RunShell({"-e", out_db.path(), "-q", query.path(), trace.path()});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("Cannot combine"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicPerfFile) {
  // --perf-file FILE -Q "SQL" -> query --perf-file FILE trace "SQL"
  auto trace = WriteSimpleSystrace();
  auto perf = base::TempFile::Create();
  auto result = RunShell(
      {"--perf-file", perf.path(), "-Q", "SELECT 200 + 61", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  // Verify perf file was written with timing data.
  std::string perf_content;
  ASSERT_TRUE(base::ReadFile(perf.path(), &perf_content));
  EXPECT_THAT(perf_content, Not(IsEmpty()));
}

TEST(TraceProcessorShellIntegrationTest, ClassicSummaryWithQuery) {
  // --summary -q file -> summarize --post-query file
  // Use computed value: 700 + 31 = 731, "731" not in input SQL.
  auto trace = WriteSimpleSystrace();
  auto query = WriteTempFile("SELECT 700 + 31 AS post_query_col;");
  auto result = RunShell({"--summary", "-q", query.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("731"));
}

TEST(TraceProcessorShellIntegrationTest, ClassicMetricsWithQuery) {
  // --run-metrics x -q file -> metrics --run x --post-query file
  // Use computed value: 800 + 19 = 819, "819" not in input SQL.
  auto trace = WriteSimpleSystrace();
  auto query = WriteTempFile("SELECT 800 + 19 AS post_query_col;");
  auto result = RunShell(
      {"--run-metrics", "android_cpu", "-q", query.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("819"));
}

// ---------------------------------------------------------------------------
// Query subcommand tests
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, QueryWithQueryFile) {
  auto trace = WriteSimpleSystrace();
  auto query = WriteTempFile("SELECT 42 AS val;");
  auto result = RunShell({"query", "-f", query.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("val"));
  EXPECT_THAT(result.out, HasSubstr("42"));
}

TEST(TraceProcessorShellIntegrationTest, QueryWithPositionalSql) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"query", trace.path(), "SELECT 99 AS num"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("num"));
  EXPECT_THAT(result.out, HasSubstr("99"));
}

TEST(TraceProcessorShellIntegrationTest, QueryWithStdinPipe) {
  auto trace = WriteSimpleSystrace();
  // Use -f - to read from stdin; RunShell uses /dev/null for stdin so this
  // will produce an empty query which should fail.
  auto result = RunShell({"query", "-f", "-", trace.path()});
  // Empty SQL from /dev/null should still succeed (no statements = ok).
  // The behaviour depends on RunQueries handling empty input.
  // Either way the process should not crash.
  EXPECT_TRUE(result.exit_code == 0 || result.exit_code == 1);
}

TEST(TraceProcessorShellIntegrationTest, QueryHelp) {
  auto result = RunShell({"query", "-h"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("query"));
  EXPECT_THAT(result.out, HasSubstr("trace_file"));
}

TEST(TraceProcessorShellIntegrationTest, QueryVersion) {
  auto result = RunShell({"query", "-v"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("Trace Processor RPC API version"));
}

TEST(TraceProcessorShellIntegrationTest, QueryWithDevFlag) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"--dev", "query", trace.path(), "SELECT 1 AS x"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("x"));
}

TEST(TraceProcessorShellIntegrationTest, QueryNoSqlError) {
  auto trace = WriteSimpleSystrace();
  // stdin is /dev/null (not a tty), so the subcommand reads empty SQL from it
  // and RunQueries returns an error about no valid SQL.
  auto result = RunShell({"query", trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, QueryNoTraceError) {
  auto result = RunShell({"query"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("trace file is required"));
}

TEST(TraceProcessorShellIntegrationTest, QueryBadTraceFile) {
  auto result = RunShell({"query", "/nonexistent_trace.pb", "SELECT 1"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, QuerySubcommandInteractive) {
  // -i runs the query then drops into the interactive shell. We prove the
  // shell actually started by having the query CREATE a table (via positional
  // SQL so stdin is NOT consumed), then piping a SELECT on that table via
  // stdin to the REPL.
  auto trace = WriteSimpleSystrace();

  base::Subprocess p;
  p.args.exec_cmd = {ShellPath(), "query", "-i", trace.path(),
                     "CREATE PERFETTO TABLE __test AS SELECT 500 AS val;"};
  p.args.stdin_mode = base::Subprocess::InputMode::kBuffer;
  p.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  p.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
  // Compute 500 + 179 = 679 in the interactive shell. "679" doesn't appear
  // in any input text, proving the shell ran and computed the result.
  p.args.input = "SELECT val + 179 AS result FROM __test;\n";
  p.Start();
  ASSERT_TRUE(p.Wait(kDefaultTestTimeoutMs));
  EXPECT_EQ(p.returncode(), 0);
  EXPECT_THAT(p.output(), HasSubstr("679"));
}

// ---------------------------------------------------------------------------
// Subcommand: interactive
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, InteractiveSubcommand) {
  // With stdin=/dev/null the REPL exits immediately.
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"interactive", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, InteractiveSubcommandWide) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"interactive", "-W", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, InteractiveSubcommandNoTraceFails) {
  auto result = RunShell({"interactive"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest,
     InteractiveSubcommandGlobalFlagBefore) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"--dev", "interactive", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

// ---------------------------------------------------------------------------
// Subcommand: server
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, ServerSubcommandStdio) {
  TraceProcessorRpcStream req;
  auto* rpc = req.add_msg();
  rpc->set_request(TraceProcessorRpc::TPM_QUERY_STREAMING);
  rpc->mutable_query_args()->set_sql_query("SELECT 1 AS x");

  base::Subprocess process({ShellPath(), "server", "stdio"});
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

TEST(TraceProcessorShellIntegrationTest, ServerSubcommandNoModeFails) {
  auto result = RunShell({"server"});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ServerSubcommandBadModeFails) {
  auto result = RunShell({"server", "badmode"});
  EXPECT_NE(result.exit_code, 0);
}

// ---------------------------------------------------------------------------
// Subcommand: summarize
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, SummarizeSubcommand) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"summarize", trace.path()});
  EXPECT_EQ(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, SummarizeSubcommandNoTraceFails) {
  auto result = RunShell({"summarize"});
  EXPECT_NE(result.exit_code, 0);
}

// ---------------------------------------------------------------------------
// Subcommand: export
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, ExportSubcommandSqlite) {
  auto trace = WriteSimpleSystrace();
  auto out_db = base::TempFile::Create();
  auto result =
      RunShell({"export", "sqlite", "-o", out_db.path(), trace.path()});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(base::FileExists(out_db.path()));
}

TEST(TraceProcessorShellIntegrationTest, ExportSubcommandNoFormatFails) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"export", trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, ExportSubcommandNoOutputFails) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"export", "sqlite", trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

// ---------------------------------------------------------------------------
// Subcommand: metrics
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, MetricsSubcommandNoRunFails) {
  auto trace = WriteSimpleSystrace();
  auto result = RunShell({"metrics", trace.path()});
  EXPECT_NE(result.exit_code, 0);
}

TEST(TraceProcessorShellIntegrationTest, NoArgsShowsSubcommandHelp) {
  // Running with no arguments should show the subcommand help (not classic).
  auto result = RunShell({});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("Commands:"));
  EXPECT_THAT(result.out, HasSubstr("query"));
}

TEST(TraceProcessorShellIntegrationTest, BadTraceFileShowsOnlyError) {
  // When a trace file doesn't exist, only the error should be shown,
  // not the full usage text.
  auto result = RunShell({"interactive", "/nonexistent_trace.pb"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_THAT(result.out, Not(HasSubstr("Usage:")));
}

TEST(TraceProcessorShellIntegrationTest, ClassicBadTraceFileShowsOnlyError) {
  // Classic path: bare nonexistent file should show only an error, not usage.
  auto result = RunShell({"/nonexistent_trace.pb"});
  EXPECT_NE(result.exit_code, 0);
  EXPECT_THAT(result.out, Not(HasSubstr("Usage:")));
}

// ---------------------------------------------------------------------------
// Classic: --metric-extension with --stdiod
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, ClassicMetricExtensionWithStdiod) {
  // Regression test: --metric-extension was silently dropped when combined
  // with --stdiod (or --httpd) after the classic-to-subcommand migration.
  // The extension SQL should be registered and queryable via RPC.

  // Create a metric extension directory with sql/ and protos/ subdirs.
  auto ext_dir = base::TempDir::Create();
  std::string sql_dir = ext_dir.path() + "/sql/";
  std::string protos_dir = ext_dir.path() + "/protos/";
  ASSERT_TRUE(base::Mkdir(sql_dir));
  ASSERT_TRUE(base::Mkdir(protos_dir));

  // Write a metric SQL file that creates a perfetto table. We verify the
  // extension loaded by querying that table via RPC.
  std::string sql_path = sql_dir + "test_ext_metric.sql";
  {
    std::string sql_content =
        "CREATE PERFETTO TABLE _test_ext_marker AS SELECT 42 AS val;";
    auto fd = base::OpenFile(sql_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_TRUE(fd);
    base::WriteAll(*fd, sql_content.data(), sql_content.size());
  }

  // Ensure cleanup of files/dirs so TempDir destructor succeeds.
  auto cleanup = base::OnScopeExit([&] {
    unlink(sql_path.c_str());
    base::Rmdir(sql_dir);
    base::Rmdir(protos_dir);
  });

  // Build RPC: first run the metric extension SQL, then query the table it
  // creates.
  TraceProcessorRpcStream req;
  auto* rpc = req.add_msg();
  rpc->set_request(TraceProcessorRpc::TPM_QUERY_STREAMING);
  rpc->mutable_query_args()->set_sql_query(
      "SELECT RUN_METRIC('test_ext/test_ext_metric.sql')");

  rpc = req.add_msg();
  rpc->set_request(TraceProcessorRpc::TPM_QUERY_STREAMING);
  rpc->mutable_query_args()->set_sql_query("SELECT val FROM _test_ext_marker");

  std::string ext_arg = ext_dir.path() + "@test_ext/";

  base::Subprocess process(
      {ShellPath(), "--stdiod", "--metric-extension", ext_arg});
  process.args.stdin_mode = base::Subprocess::InputMode::kBuffer;
  process.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.input = req.SerializeAsString();
  process.Start();
  ASSERT_TRUE(process.Wait(kDefaultTestTimeoutMs));
  EXPECT_EQ(process.returncode(), 0);

  TraceProcessorRpcStream stream;
  stream.ParseFromString(process.output());
  ASSERT_THAT(stream.msg(), SizeIs(2));

  // First response: RUN_METRIC should succeed.
  ASSERT_EQ(stream.msg()[0].response(), TraceProcessorRpc::TPM_QUERY_STREAMING);

  // Second response: the table created by the metric extension should exist
  // and contain val=42.
  ASSERT_EQ(stream.msg()[1].response(), TraceProcessorRpc::TPM_QUERY_STREAMING);
  ASSERT_THAT(stream.msg()[1].query_result().batch(), SizeIs(1));
  EXPECT_THAT(stream.msg()[1].query_result().batch()[0].varint_cells(),
              ElementsAre(42));
}

// ---------------------------------------------------------------------------
// Classic: --add-sql-package with --stdiod
// ---------------------------------------------------------------------------

TEST(TraceProcessorShellIntegrationTest, ClassicAddSqlPackageWithStdiod) {
  // Verify --add-sql-package works with --stdiod (server subcommand).
  // The package SQL should be includable via INCLUDE PERFETTO MODULE.

  // Create a package directory with a SQL file.
  auto pkg_dir = base::TempDir::Create();
  std::string sql_path = pkg_dir.path() + "/hello.sql";
  {
    std::string sql_content =
        "CREATE PERFETTO TABLE _test_pkg_marker AS SELECT 99 AS val;";
    auto fd = base::OpenFile(sql_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_TRUE(fd);
    base::WriteAll(*fd, sql_content.data(), sql_content.size());
  }

  auto cleanup = base::OnScopeExit([&] { unlink(sql_path.c_str()); });

  // Extract package name from dir basename. The dir path ends with a random
  // name like /tmp/perfetto-XXXXXX, so the package name is that basename.
  // Override with @testpkg for a stable name.
  std::string pkg_arg = pkg_dir.path() + "@testpkg";

  TraceProcessorRpcStream req;
  // Include the module, then query the table it creates.
  auto* rpc = req.add_msg();
  rpc->set_request(TraceProcessorRpc::TPM_QUERY_STREAMING);
  rpc->mutable_query_args()->set_sql_query(
      "INCLUDE PERFETTO MODULE testpkg.hello");

  rpc = req.add_msg();
  rpc->set_request(TraceProcessorRpc::TPM_QUERY_STREAMING);
  rpc->mutable_query_args()->set_sql_query("SELECT val FROM _test_pkg_marker");

  base::Subprocess process(
      {ShellPath(), "--stdiod", "--add-sql-package", pkg_arg});
  process.args.stdin_mode = base::Subprocess::InputMode::kBuffer;
  process.args.stdout_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
  process.args.input = req.SerializeAsString();
  process.Start();
  ASSERT_TRUE(process.Wait(kDefaultTestTimeoutMs));
  EXPECT_EQ(process.returncode(), 0);

  TraceProcessorRpcStream stream;
  stream.ParseFromString(process.output());
  ASSERT_THAT(stream.msg(), SizeIs(2));

  // First response: INCLUDE should succeed.
  ASSERT_EQ(stream.msg()[0].response(), TraceProcessorRpc::TPM_QUERY_STREAMING);

  // Second response: the table should exist with val=99.
  ASSERT_EQ(stream.msg()[1].response(), TraceProcessorRpc::TPM_QUERY_STREAMING);
  ASSERT_THAT(stream.msg()[1].query_result().batch(), SizeIs(1));
  EXPECT_THAT(stream.msg()[1].query_result().batch()[0].varint_cells(),
              ElementsAre(99));
}

// ---------------------------------------------------------------------------
// Subcommand: convert (wraps traceconv)
// ---------------------------------------------------------------------------

namespace {
// Path to a real, small Perfetto trace shipped in test/data.
std::string HeapprofdTracePath() {
  return base::GetTestDataPath(
      "test/data/heapprofd_standalone_client_example-trace");
}
}  // namespace

TEST(TraceProcessorShellIntegrationTest, ConvertBundleWithProguardMap) {
  auto mapping = WriteTempFile("com.example.Foo -> a.a:\n");
  auto out_dir = base::TempDir::Create();
  std::string out_path = out_dir.path() + "/bundle.tar";

  auto result = RunShell({"convert", "bundle", "--no-auto-symbol-paths",
                          "--proguard-map", "com.example=" + mapping.path(),
                          HeapprofdTracePath(), out_path});
  EXPECT_EQ(result.exit_code, 0);

  std::string tar_bytes;
  ASSERT_TRUE(base::ReadFile(out_path, &tar_bytes));
  // USTAR embeds filenames in 100-byte header fields, so substring matching is
  // sufficient to check archive membership.
  EXPECT_THAT(tar_bytes, HasSubstr("trace.perfetto"));
  EXPECT_THAT(tar_bytes, HasSubstr("deobfuscation.pb"));
  unlink(out_path.c_str());
}

TEST(TraceProcessorShellIntegrationTest, ConvertBundleRepeatedProguardMap) {
  auto m1 = WriteTempFile("com.example.Foo -> a.a:\n");
  auto m2 = WriteTempFile("com.example.Bar -> b.b:\n");
  auto out_dir = base::TempDir::Create();
  std::string out_path = out_dir.path() + "/bundle.tar";

  auto result = RunShell({"convert", "bundle", "--no-auto-symbol-paths",
                          "--proguard-map", "com.example.one=" + m1.path(),
                          "--proguard-map", "com.example.two=" + m2.path(),
                          HeapprofdTracePath(), out_path});
  EXPECT_EQ(result.exit_code, 0);

  std::string tar_bytes;
  ASSERT_TRUE(base::ReadFile(out_path, &tar_bytes));
  EXPECT_THAT(tar_bytes, HasSubstr("deobfuscation.pb"));
  unlink(out_path.c_str());
}

TEST(TraceProcessorShellIntegrationTest, ConvertBundleMissingProguardMapFails) {
  auto out_dir = base::TempDir::Create();
  std::string out_path = out_dir.path() + "/bundle.tar";

  auto result = RunShell(
      {"convert", "bundle", "--no-auto-symbol-paths", "--proguard-map",
       "com.example=/nonexistent/mapping.txt", HeapprofdTracePath(), out_path});
  EXPECT_NE(result.exit_code, 0);
  unlink(out_path.c_str());
}

TEST(TraceProcessorShellIntegrationTest, ConvertHelpShowsProguardMap) {
  auto result = RunShell({"help", "convert"});
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_THAT(result.out, HasSubstr("proguard-map"));
  EXPECT_THAT(result.out, HasSubstr("symbol-paths"));
  EXPECT_THAT(result.out, HasSubstr("no-auto-proguard-maps"));
}

TEST(TraceProcessorShellIntegrationTest, ConvertBundleNoAutoProguardMaps) {
  auto mapping = WriteTempFile("com.example.Foo -> a.a:\n");
  auto out_dir = base::TempDir::Create();
  std::string out_path = out_dir.path() + "/bundle.tar";

  auto result = RunShell({"convert", "bundle", "--no-auto-symbol-paths",
                          "--no-auto-proguard-maps", "--proguard-map",
                          "com.example=" + mapping.path(), HeapprofdTracePath(),
                          out_path});
  EXPECT_EQ(result.exit_code, 0);

  std::string tar_bytes;
  ASSERT_TRUE(base::ReadFile(out_path, &tar_bytes));
  // Explicit --proguard-map still wins when auto-discovery is disabled.
  EXPECT_THAT(tar_bytes, HasSubstr("deobfuscation.pb"));
  unlink(out_path.c_str());
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
