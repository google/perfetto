/*
 * Copyright (C) 2026 The Android Open Source Project
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

// End-to-end tests for `--debuginfod`: a local HTTP server stands in for a
// debuginfod service and trace_processor_shell is driven as a subprocess.
// Needs curl and llvm-symbolizer; the tests skip when either is missing.

#include "perfetto/base/build_config.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/http/http_server.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/thread_task_runner.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"
#include "protos/perfetto/trace/clock_snapshot.gen.h"
#include "protos/perfetto/trace/interned_data/interned_data.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.gen.h"
#include "src/base/test/utils.h"
#include "test/gtest_and_gmock.h"
#include "test/test_helper.h"

extern "C" char** environ;

namespace perfetto {
namespace {

using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;

constexpr char kBuildIdHex[] = "f7558cfad3e9e2ff6cafcb0fd8442a322210ba6e";
constexpr char kSymbol[] = "TestFunctionToSymbolize";

std::string HexToBytes(const std::string& hex) {
  std::string bytes;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
    bytes.push_back(
        static_cast<char>(strtol(hex.substr(i, 2).c_str(), nullptr, 16)));
  return bytes;
}

// A minimal profile whose only frame lives in a mapping identified by
// kBuildIdHex, so it can only be symbolized through debuginfod.
std::string BuildTrace() {
  protos::gen::Trace trace;
  auto* clocks = trace.add_packet()->mutable_clock_snapshot();
  for (uint32_t clock_id : {3u, 5u, 6u}) {
    auto* clock = clocks->add_clocks();
    clock->set_clock_id(clock_id);
    clock->set_timestamp(0);
  }
  auto* packet = trace.add_packet();
  packet->set_trusted_packet_sequence_id(1);
  packet->set_incremental_state_cleared(true);
  auto* thread = packet->mutable_thread_descriptor();
  thread->set_pid(1);
  thread->set_tid(1);
  auto* interned = packet->mutable_interned_data();
  auto* build_id = interned->add_build_ids();
  build_id->set_iid(1);
  build_id->set_str(HexToBytes(kBuildIdHex));
  auto* path = interned->add_mapping_paths();
  path->set_iid(1);
  path->set_str("remote-only");
  auto* mapping = interned->add_mappings();
  mapping->set_iid(1);
  mapping->set_build_id(1);
  mapping->add_path_string_ids(1);
  mapping->set_start(0x100000);
  mapping->set_end(0x110000);
  mapping->set_load_bias(0);
  mapping->set_exact_offset(4096);
  auto* frame = interned->add_frames();
  frame->set_iid(1);
  frame->set_mapping_id(1);
  frame->set_rel_pc(4400);
  auto* callstack = interned->add_callstacks();
  callstack->set_iid(1);
  callstack->add_frame_ids(1);
  auto* sample = trace.add_packet();
  sample->set_trusted_packet_sequence_id(1);
  auto* samples = sample->mutable_streaming_profile_packet();
  samples->add_callstack_iid(1);
  samples->add_timestamp_delta_us(1);
  return trace.SerializeAsString();
}

int FindFreePort() {
  auto sock = base::UnixSocketRaw::CreateMayFail(base::SockFamily::kInet,
                                                 base::SockType::kStream);
  PERFETTO_CHECK(sock && sock.Bind("127.0.0.1:0"));
  std::string addr = sock.GetSockAddr();
  int port = atoi(addr.substr(addr.rfind(':') + 1).c_str());
  PERFETTO_CHECK(port > 0);
  return port;
}

// Serves the fixture binary under path prefixes that select a behaviour:
// /ok, /missing (404), /redirect (302 to /ok), /bad (not an ELF), /wrong
// (ELF with a different build ID) and /slow (2s delay before responding).
class DebugFileServer : public base::HttpRequestHandler {
 public:
  explicit DebugFileServer(std::string binary)
      : binary_(std::move(binary)), port_(FindFreePort()) {
    wrong_binary_ = binary_;
    size_t pos = wrong_binary_.find(HexToBytes(kBuildIdHex));
    PERFETTO_CHECK(pos != std::string::npos);
    wrong_binary_.replace(pos, 20, std::string(20, '\0'));
    runner_.PostTaskAndWaitForTesting([this] {
      server_.reset(new base::HttpServer(runner_.get(), this));
      server_->SetQuiet(true);
      server_->Start("127.0.0.1", port_);
    });
  }
  ~DebugFileServer() override {
    runner_.PostTaskAndWaitForTesting([this] { server_.reset(); });
  }

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

  std::vector<std::string> requests() {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_;
  }
  void ClearRequests() {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.clear();
  }

 private:
  void OnHttpRequest(const base::HttpRequest& req) override {
    std::string path = req.uri.ToStdString();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      requests_.push_back(path);
    }
    if (base::StartsWith(path, "/missing/"))
      return req.conn->SendResponseAndClose("404 Not Found");
    if (base::StartsWith(path, "/slow/"))
      std::this_thread::sleep_for(std::chrono::seconds(2));
    if (base::StartsWith(path, "/redirect/")) {
      std::string location =
          "Location: /ok/" + path.substr(strlen("/redirect/"));
      return req.conn->SendResponseAndClose("302 Found", {location.c_str()});
    }
    const std::string* data = &binary_;
    std::string not_elf = "not an ELF file";
    if (base::StartsWith(path, "/bad/"))
      data = &not_elf;
    if (base::StartsWith(path, "/wrong/"))
      data = &wrong_binary_;
    req.conn->SendResponseAndClose("200 OK", {}, base::StringView(*data));
  }
  uint8_t* OnHttpRequestBody(const base::HttpRequest&, size_t size) override {
    body_.resize(size);
    return body_.data();
  }
  uint8_t* OnWebsocketPayload(base::HttpServerConnection*, size_t) override {
    return nullptr;
  }
  void OnHttpConnectionClosed(base::HttpServerConnection*) override {}
  void OnWebsocketMessage(const base::WebsocketMessage&) override {}

  std::string binary_;
  std::string wrong_binary_;
  std::vector<uint8_t> body_;
  int port_;
  std::mutex mutex_;
  std::vector<std::string> requests_;
  base::ThreadTaskRunner runner_ = base::ThreadTaskRunner::CreateAndStart();
  std::unique_ptr<base::HttpServer> server_;
};

struct ShellResult {
  int exit_code = -1;
  std::string out;
  std::string err;
};

class DebuginfodIntegrationTest : public testing::Test {
 protected:
  void SetUp() override {
    if (!ToolRuns("curl --version") || !ToolRuns("llvm-symbolizer --version"))
      GTEST_SKIP() << "curl and llvm-symbolizer are required";
    std::string binary;
    ASSERT_TRUE(base::ReadFile(
        base::GetTestDataPath("test/data/test_symbolizer_binary"), &binary));
    server_.reset(new DebugFileServer(binary));
    root_ = temp_dir_.path();
    cache_ = root_ + "/cache";
    bundle_ = root_ + "/bundle.tar";
    trace_ = root_ + "/trace.pftrace";
    std::string trace = BuildTrace();
    base::WriteAll(*base::OpenFile(trace_, O_CREAT | O_WRONLY, 0644),
                   trace.data(), trace.size());
  }

  // Removes exactly what a test can create; anything else left behind is
  // caught by TempDir, which only removes an empty directory.
  void TearDown() override {
    std::string entry_dir = cache_ + "/" + kBuildIdHex;
    for (const std::string& file :
         {entry_dir + "/debuginfo", root_ + "/local/binary", root_ + "/.curlrc",
          bundle_, trace_}) {
      base::Unlink(file.c_str());
    }
    for (const std::string& dir : {entry_dir, cache_, root_ + "/local"})
      base::Rmdir(dir);
  }

  // The child environment: the caller's, minus every variable that steers
  // symbolization, plus |extra|. The hermetic llvm-symbolizer is preferred
  // when the build has one.
  std::vector<std::string> Environment(
      std::initializer_list<std::string> extra = {}) {
    static const char* const kDropped[] = {
        "DEBUGINFOD_URLS",      "DEBUGINFOD_CACHE_PATH", "LLVM_SYMBOLIZER_OPTS",
        "PERFETTO_BINARY_PATH", "BREAKPAD_SYMBOL_DIR",   "CURL_HOME"};
    std::vector<std::string> env;
    for (char** entry = environ; *entry; ++entry) {
      std::string var = *entry;
      std::string name = var.substr(0, var.find('='));
      bool dropped = false;
      for (const char* d : kDropped)
        dropped |= name == d;
      if (dropped)
        continue;
      if (name == "PATH")
        var = "PATH=" + ToolPath() + var.substr(5);
      env.push_back(var);
    }
    env.push_back("NO_PROXY=127.0.0.1");
    env.push_back("no_proxy=127.0.0.1");
    env.insert(env.end(), extra.begin(), extra.end());
    return env;
  }

  ShellResult RunShell(std::vector<std::string> args,
                       std::initializer_list<std::string> extra_env = {}) {
    base::Subprocess p;
    p.args.exec_cmd.push_back(base::GetCurExecutableDir() +
                              "/trace_processor_shell");
    p.args.exec_cmd.insert(p.args.exec_cmd.end(), args.begin(), args.end());
    p.args.env = Environment(extra_env);
    p.args.stdin_mode = base::Subprocess::InputMode::kDevNull;
    base::TempFile stdout_file = base::TempFile::Create();
    p.args.stdout_mode = base::Subprocess::OutputMode::kFd;
    p.args.out_fd.reset(dup(stdout_file.fd()));
    p.args.stderr_mode = base::Subprocess::OutputMode::kBuffer;
    p.Start();
    PERFETTO_CHECK(p.Wait(kDefaultTestTimeoutMs));
    ShellResult result;
    result.exit_code = p.returncode();
    result.err = WithoutDebugLogs(p.output());
    base::ReadFile(stdout_file.path(), &result.out);
    return result;
  }

  ShellResult Bundle(std::vector<std::string> args,
                     std::initializer_list<std::string> extra_env = {}) {
    std::vector<std::string> cmd = {"bundle", "--no-auto-symbol-paths",
                                    "--no-auto-proguard-maps",
                                    "--debuginfod-cache-path", cache_};
    cmd.insert(cmd.end(), args.begin(), args.end());
    cmd.push_back(trace_);
    cmd.push_back(bundle_);
    auto result = RunShell(cmd, extra_env);
    EXPECT_EQ(result.exit_code, 0) << result.err;
    return result;
  }

  void ExpectSymbolized(const std::string& trace) {
    auto result = RunShell(
        {"query", "--quiet", trace, "SELECT name FROM stack_profile_symbol"});
    EXPECT_EQ(result.exit_code, 0) << result.err;
    EXPECT_THAT(result.out, HasSubstr(kSymbol));
  }

  std::vector<std::string> CacheFiles() {
    std::vector<std::string> files;
    if (base::FileExists(cache_))
      base::ListFilesRecursive(cache_, files);
    return files;
  }

  std::string OkRequest() {
    return std::string("/ok/buildid/") + kBuildIdHex + "/debuginfo";
  }

  std::string CacheEntry() { return cache_ + "/" + kBuildIdHex + "/debuginfo"; }

  base::TempDir temp_dir_ = base::TempDir::Create();
  std::unique_ptr<DebugFileServer> server_;
  std::string root_;
  std::string cache_;
  std::string bundle_;
  std::string trace_;

 private:
  static std::string ToolPath() {
    std::string dir = base::GetTestDataPath("buildtools/linux64/clang/bin");
    if (base::FileExists(dir + "/llvm-symbolizer"))
      return dir + ":";
    return "";
  }

  // Through the shell so the child PATH, not the test's, is searched.
  bool ToolRuns(const std::string& cmd) {
    base::Subprocess p({"/bin/sh", "-c", cmd});
    p.args.env = Environment();
    p.args.stdin_mode = base::Subprocess::InputMode::kDevNull;
    p.args.stdout_mode = base::Subprocess::OutputMode::kDevNull;
    p.args.stderr_mode = base::Subprocess::OutputMode::kDevNull;
    return p.Call(kDefaultTestTimeoutMs);
  }
};

TEST_F(DebuginfodIntegrationTest, RemoteOnlyAndCacheReuse) {
  auto result =
      Bundle({"--debuginfod", "--debuginfod-urls", server_->url() + "/ok"});
  EXPECT_THAT(result.err, HasSubstr("1 downloaded"));
  ExpectSymbolized(bundle_);
  EXPECT_THAT(server_->requests(), ElementsAre(OkRequest()));

  server_->ClearRequests();
  result =
      Bundle({"--debuginfod", "--debuginfod-urls", server_->url() + "/ok"});
  EXPECT_THAT(result.err, HasSubstr("1 from cache"));
  EXPECT_THAT(server_->requests(), IsEmpty());
}

TEST_F(DebuginfodIntegrationTest, DisabledWarnsEvenWhenQuiet) {
  auto result =
      Bundle({"--quiet"}, {"DEBUGINFOD_URLS=" + server_->url() + "/ok"});
  EXPECT_THAT(result.err, HasSubstr("ignored"));
  EXPECT_THAT(result.err, HasSubstr("--debuginfod"));
  EXPECT_THAT(server_->requests(), IsEmpty());
  EXPECT_FALSE(base::FileExists(cache_));
}

TEST_F(DebuginfodIntegrationTest, CliOverridesEnvironmentAndLlvmOptions) {
  auto result = Bundle(
      {"--debuginfod", "--debuginfod-urls", server_->url() + "/redirect"},
      {"DEBUGINFOD_URLS=" + server_->url() + "/missing",
       "DEBUGINFOD_CACHE_PATH=" + root_ + "/wrong-cache",
       "LLVM_SYMBOLIZER_OPTS=--output-style=GNU --debuginfod "
       "--invalid-option"});
  EXPECT_THAT(result.err, HasSubstr("LLVM_SYMBOLIZER_OPTS is ignored"));
  ExpectSymbolized(bundle_);
  EXPECT_FALSE(base::FileExists(root_ + "/wrong-cache"));
  for (const auto& request : server_->requests())
    EXPECT_THAT(request, Not(HasSubstr("/missing/")));
}

TEST_F(DebuginfodIntegrationTest, InvalidResponseFallsBack) {
  auto result =
      Bundle({"--debuginfod", "--debuginfod-urls",
              server_->url() + "/bad\t" + server_->url() + "/ok", "--verbose"});
  EXPECT_THAT(result.err, HasSubstr("invalid debug file"));
  ExpectSymbolized(bundle_);
  EXPECT_EQ(server_->requests().size(), 2u);
  for (const auto& file : CacheFiles())
    EXPECT_THAT(file, Not(HasSubstr(".tmp.")));
}

TEST_F(DebuginfodIntegrationTest, WrongBuildIdFallsBack) {
  Bundle({"--debuginfod", "--debuginfod-urls",
          server_->url() + "/wrong " + server_->url() + "/ok"});
  ExpectSymbolized(bundle_);
  EXPECT_EQ(server_->requests().size(), 2u);
}

TEST_F(DebuginfodIntegrationTest, InvalidCacheIsReplaced) {
  ASSERT_TRUE(base::Mkdir(cache_));
  ASSERT_TRUE(base::Mkdir(cache_ + "/" + kBuildIdHex));
  base::WriteAll(*base::OpenFile(CacheEntry(), O_CREAT | O_WRONLY, 0644),
                 "incomplete", 10);
  Bundle({"--debuginfod", "--debuginfod-urls", server_->url() + "/ok"});
  ExpectSymbolized(bundle_);
  EXPECT_EQ(server_->requests().size(), 1u);
  std::string entry, fixture;
  ASSERT_TRUE(base::ReadFile(CacheEntry(), &entry));
  ASSERT_TRUE(base::ReadFile(
      base::GetTestDataPath("test/data/test_symbolizer_binary"), &fixture));
  EXPECT_EQ(entry, fixture);
}

TEST_F(DebuginfodIntegrationTest, CurlConfigCannotAddRequests) {
  std::string curlrc = "url = \"" + server_->url() + "/missing\"";
  base::WriteAll(*base::OpenFile(root_ + "/.curlrc", O_CREAT | O_WRONLY, 0644),
                 curlrc.data(), curlrc.size());
  Bundle({"--debuginfod", "--debuginfod-urls", server_->url() + "/ok"},
         {"CURL_HOME=" + root_});
  ExpectSymbolized(bundle_);
  EXPECT_THAT(server_->requests(), ElementsAre(OkRequest()));
}

TEST_F(DebuginfodIntegrationTest, FailedDownloadDoesNotPublishCacheEntry) {
  auto result =
      Bundle({"--debuginfod", "--debuginfod-urls", server_->url() + "/bad"});
  EXPECT_THAT(result.err, HasSubstr("1 failed"));
  EXPECT_FALSE(base::FileExists(CacheEntry()));
  for (const auto& file : CacheFiles())
    EXPECT_THAT(file, Not(HasSubstr(".tmp.")));
}

TEST_F(DebuginfodIntegrationTest, LocalResultPreventsDownload) {
  std::string local = root_ + "/local";
  ASSERT_TRUE(base::Mkdir(local));
  std::string fixture;
  ASSERT_TRUE(base::ReadFile(
      base::GetTestDataPath("test/data/test_symbolizer_binary"), &fixture));
  base::WriteAll(*base::OpenFile(local + "/binary", O_CREAT | O_WRONLY, 0644),
                 fixture.data(), fixture.size());
  Bundle({"--debuginfod", "--debuginfod-urls", server_->url() + "/ok",
          "--symbol-paths", local});
  ExpectSymbolized(bundle_);
  EXPECT_THAT(server_->requests(), IsEmpty());
}

TEST_F(DebuginfodIntegrationTest, StallTimeoutFallsBack) {
  auto result = Bundle(
      {"--debuginfod", "--debuginfod-stall-timeout", "1", "--debuginfod-urls",
       server_->url() + "/slow " + server_->url() + "/ok", "--verbose"});
  EXPECT_THAT(result.err, HasSubstr("server unreachable"));
  ExpectSymbolized(bundle_);
}

TEST_F(DebuginfodIntegrationTest, SymbolizeUtility) {
  auto result =
      RunShell({"util", "symbolize", "--debuginfod", "--debuginfod-urls",
                server_->url() + "/ok", "--debuginfod-cache-path", cache_,
                "--quiet", trace_});
  EXPECT_EQ(result.exit_code, 0) << result.err;
  EXPECT_THAT(result.out, HasSubstr(kSymbol));
  EXPECT_THAT(result.err, IsEmpty());
}

TEST_F(DebuginfodIntegrationTest, QueryWithoutLocalBinaries) {
  auto result = RunShell(
      {"query", "--quiet", trace_, "SELECT name FROM stack_profile_symbol",
       "--debuginfod", "--debuginfod-urls", server_->url() + "/ok",
       "--debuginfod-cache-path", cache_});
  EXPECT_EQ(result.exit_code, 0) << result.err;
  EXPECT_THAT(result.out, HasSubstr(kSymbol));
  EXPECT_THAT(result.err, IsEmpty());
}

}  // namespace
}  // namespace perfetto

#endif  // !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
