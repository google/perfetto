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

#include "src/trace_processor/util/symbolizer/symbolize_database.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "protos/perfetto/trace/interned_data/interned_data.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.gen.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::profiling {
namespace {

void AddProfile(protos::gen::Trace* trace,
                uint32_t sequence_id,
                uint64_t mapping_start,
                uint64_t exact_offset,
                const std::vector<uint64_t>& rel_pcs) {
  auto* intern_packet = trace->add_packet();
  intern_packet->set_trusted_packet_sequence_id(sequence_id);
  intern_packet->set_incremental_state_cleared(true);

  auto* thread = intern_packet->mutable_thread_descriptor();
  thread->set_pid(static_cast<int32_t>(sequence_id));
  thread->set_tid(static_cast<int32_t>(sequence_id));

  auto* interned = intern_packet->mutable_interned_data();
  auto* build_id = interned->add_build_ids();
  build_id->set_iid(1);
  build_id->set_str("build-id");

  auto* mapping = interned->add_mappings();
  mapping->set_iid(1);
  mapping->set_build_id(1);
  mapping->set_start(mapping_start);
  mapping->set_end(mapping_start + 0x10000);
  mapping->set_exact_offset(exact_offset);

  auto* callstack = interned->add_callstacks();
  callstack->set_iid(1);
  for (size_t i = 0; i < rel_pcs.size(); ++i) {
    auto* frame = interned->add_frames();
    frame->set_iid(i + 1);
    frame->set_mapping_id(1);
    frame->set_rel_pc(rel_pcs[i]);
    callstack->add_frame_ids(i + 1);
  }

  auto* sample_packet = trace->add_packet();
  sample_packet->set_trusted_packet_sequence_id(sequence_id);
  auto* samples = sample_packet->mutable_streaming_profile_packet();
  samples->add_callstack_iid(1);
  samples->add_timestamp_delta_us(1);
}

std::unique_ptr<trace_processor::TraceProcessor> LoadTrace(
    const protos::gen::Trace& trace) {
  auto tp = trace_processor::TraceProcessor::CreateInstance({});
  std::string serialized = trace.SerializeAsString();
  std::unique_ptr<uint8_t[]> data(new uint8_t[serialized.size()]);
  memcpy(data.get(), serialized.data(), serialized.size());
  PERFETTO_CHECK(tp->Parse(std::move(data), serialized.size()).ok());
  tp->NotifyEndOfFile();
  return tp;
}

void WriteSymbols(const std::string& path, const std::string& contents) {
  auto fd = base::OpenFile(path, O_CREAT | O_WRONLY, 0600);
  ASSERT_TRUE(fd);
  ASSERT_EQ(base::WriteAll(*fd, contents.data(), contents.size()),
            static_cast<ssize_t>(contents.size()));
}

#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
TEST(DebuginfodOptionsTest, ExplicitValuesAndWhitespace) {
  DebuginfodOptions options;
  options.enabled = true;
  options.urls = "  https://one.example/\t\nhttp://two.example/debug/ ";
  options.cache_path = "/chosen/cache";
  options.connect_timeout = "2";
  options.stall_timeout = "3";
  DebuginfodConfig config;
  std::string warnings;
  ASSERT_TRUE(ResolveDebuginfodOptions(options, &config, &warnings).ok());
  EXPECT_THAT(config.urls, testing::ElementsAre("https://one.example",
                                                "http://two.example/debug"));
  EXPECT_EQ(config.cache_path, "/chosen/cache");
  EXPECT_EQ(config.connect_timeout_seconds, 2u);
  EXPECT_EQ(config.stall_timeout_seconds, 3u);
}

#endif

TEST(DebuginfodOptionsTest, ConfigurationDoesNotEnableDownloads) {
  DebuginfodOptions options;
  options.urls = "https://one.example";
  options.cache_path = "/chosen/cache";
  DebuginfodConfig config;
  std::string warnings;
  ASSERT_TRUE(ResolveDebuginfodOptions(options, &config, &warnings).ok());
  EXPECT_TRUE(config.urls.empty());
  EXPECT_THAT(warnings, testing::HasSubstr("--debuginfod"));
}

TEST(DebuginfodOptionsTest, RejectsEmptyOverridesAndInvalidValues) {
  DebuginfodOptions options;
  options.enabled = true;
  options.urls = "";
  options.cache_path = "/chosen/cache";
  DebuginfodConfig config;
  std::string warnings;
  EXPECT_FALSE(ResolveDebuginfodOptions(options, &config, &warnings).ok());
  for (const char* url :
       {"file:///tmp/debug", "https://host?q=x", "https://host#fragment"}) {
    config = {};
    options.urls = url;
    EXPECT_FALSE(ResolveDebuginfodOptions(options, &config, &warnings).ok());
  }
  options.urls = "https://one.example";
  for (const char* timeout : {"0", "-1", "1.5", "bad", "4294967296"}) {
    config = {};
    options.connect_timeout = timeout;
    EXPECT_FALSE(ResolveDebuginfodOptions(options, &config, &warnings).ok());
  }
  config = {};
  options.connect_timeout = "5";
  options.cache_path = "";
  EXPECT_FALSE(ResolveDebuginfodOptions(options, &config, &warnings).ok());
}

TEST(SymbolizeDatabaseTest, CoalescesEquivalentMappingsAndAddresses) {
  protos::gen::Trace trace;
  AddProfile(&trace, 1, 0x100000, 0, {0x10, 0x20});
  AddProfile(&trace, 2, 0x200000, 0, {0x10, 0x20});

  auto tp = LoadTrace(trace);
  std::vector<UnsymbolizedFrames> frames = CollectUnsymbolizedFrames(tp.get());

  ASSERT_EQ(frames.size(), 1u);
  EXPECT_THAT(frames[0].rel_pcs, testing::ElementsAre(0x10u, 0x20u));
  EXPECT_EQ(frames[0].frame_count, 4u);
}

TEST(SymbolizeDatabaseTest, KeepsAddressCorrectionGroupsSeparate) {
  protos::gen::Trace trace;
  AddProfile(&trace, 1, 0x100000, 0, {0x10, 0x20});
  AddProfile(&trace, 2, 0x200000, 0x1000, {0x10, 0x20});

  auto tp = LoadTrace(trace);
  std::vector<UnsymbolizedFrames> frames = CollectUnsymbolizedFrames(tp.get());

  ASSERT_EQ(frames.size(), 2u);
  EXPECT_THAT(frames[0].rel_pcs, testing::ElementsAre(0x10u, 0x20u));
  EXPECT_THAT(frames[1].rel_pcs, testing::ElementsAre(0x10u, 0x20u));
  EXPECT_EQ(frames[0].frame_count, 2u);
  EXPECT_EQ(frames[1].frame_count, 2u);
}

TEST(SymbolizeDatabaseTest,
     FirstUsableSourceWinsAndFallbackFillsMissingAddresses) {
  protos::gen::Trace trace;
  AddProfile(&trace, 1, 0x100000, 0, {0x10, 0x20, 0x30});
  AddProfile(&trace, 2, 0x200000, 0, {0x10, 0x20, 0x30});
  auto tp = LoadTrace(trace);
  auto first = base::TempDir::Create();
  auto second = base::TempDir::Create();
  std::string filename = "/" + base::ToHex("build-id") + ".breakpad";
  auto cleanup = base::OnScopeExit([&] {
    base::Unlink((first.path() + filename).c_str());
    base::Unlink((second.path() + filename).c_str());
  });
  WriteSymbols(first.path() + filename,
               "MODULE Linux x86_64 01234567 test.so\n"
               "FUNC 10 1 0 first\n");
  WriteSymbols(second.path() + filename,
               "MODULE Linux x86_64 01234567 test.so\n"
               "FUNC 10 1 0 must_not_replace_first\n"
               "FUNC 20 1 0 fallback\n");
  SymbolizerConfig config;
  config.breakpad_paths = {first.path(), second.path(), second.path()};
  auto result = SymbolizeDatabase(tp.get(), config);
  ASSERT_EQ(result.successful_mappings.size(), 2u);
  EXPECT_EQ(result.successful_mappings[0].frame_count, 2u);
  EXPECT_EQ(result.successful_mappings[1].frame_count, 2u);
  ASSERT_EQ(result.failed_mappings.size(), 1u);
  EXPECT_EQ(result.failed_mappings[0].frame_count, 2u);
  EXPECT_EQ(result.failed_mappings[0].frames_without_symbols, 2u);
  auto summary = FormatSymbolizationSummary(result, false, false);
  EXPECT_THAT(summary,
              testing::HasSubstr(
                  "4 of 6 frames symbolized, 2 could not be symbolized"));
  EXPECT_THAT(summary, testing::Not(testing::HasSubstr(first.path())));
  EXPECT_THAT(FormatSymbolizationSummary(result, true, false),
              testing::HasSubstr("binary found, but it has no symbol"));

  protos::gen::Trace symbols;
  ASSERT_TRUE(symbols.ParseFromString(result.symbols));
  ASSERT_EQ(symbols.packet_size(), 2);
  const auto& first_module = symbols.packet()[0].module_symbols();
  const auto& fallback_module = symbols.packet()[1].module_symbols();
  ASSERT_EQ(first_module.address_symbols_size(), 1);
  ASSERT_EQ(fallback_module.address_symbols_size(), 1);
  EXPECT_EQ(first_module.address_symbols()[0].address(), 0x10u);
  EXPECT_EQ(first_module.address_symbols()[0].lines()[0].function_name(),
            "first");
  EXPECT_EQ(fallback_module.address_symbols()[0].address(), 0x20u);
  EXPECT_EQ(fallback_module.address_symbols()[0].lines()[0].function_name(),
            "fallback");
}

TEST(SymbolizeDatabaseTest, MissingSourcesStillCountAllFrames) {
  protos::gen::Trace trace;
  AddProfile(&trace, 1, 0x100000, 0, {0x10, 0x20});
  auto tp = LoadTrace(trace);
  auto result = SymbolizeDatabase(tp.get(), {});
  ASSERT_EQ(result.failed_mappings.size(), 1u);
  EXPECT_EQ(result.failed_mappings[0].frame_count, 2u);
  EXPECT_EQ(result.failed_mappings[0].frames_without_symbols, 0u);
  EXPECT_THAT(FormatSymbolizationSummary(result, false, false),
              testing::HasSubstr(
                  "0 of 2 frames symbolized, 2 could not be symbolized"));
  // Both reports must tell the reader how to get something searched.
  EXPECT_THAT(FormatSymbolizationSummary(result, false, false),
              testing::HasSubstr("enable automatic symbol path discovery"));
  std::string verbose = FormatSymbolizationSummary(result, true, false);
  EXPECT_THAT(verbose, testing::HasSubstr("To fix this:"));
  EXPECT_THAT(verbose, testing::HasSubstr("pass --symbol-paths"));
  EXPECT_THAT(verbose,
              testing::HasSubstr("enable automatic symbol path discovery"));
}

TEST(SymbolizeDatabaseTest, SuccessfulAndEmptySummariesAreVisible) {
  SymbolizerResult result;
  EXPECT_EQ(FormatSymbolizationSummary(result, false, false),
            "Symbolization: no native frames to symbolize.\n");
  result.successful_mappings.push_back(
      {"lib.so", "build-id", "/symbols/lib.so", 5, {}});
  EXPECT_EQ(FormatSymbolizationSummary(result, false, false),
            "Symbolization: all 5 frames symbolized.\n");
  EXPECT_THAT(FormatSymbolizationSummary(result, true, false),
              testing::HasSubstr("/symbols/lib.so"));
}

TEST(SymbolizeDatabaseTest, DebuginfodOutcomesAreReportedLikePaths) {
  SymbolizerResult result;
  result.debuginfod.enabled = true;
  result.debuginfod.downloads = 1;
  result.debuginfod.not_found = 1;
  result.debuginfod.failed = 1;
  result.debuginfod.unreachable_servers = {"http://down.example"};
  result.successful_mappings.push_back(
      {"/usr/lib/liba.so",
       "a",
       "/cache/aa/debuginfo",
       2,
       {{"http://down.example", SymbolPathError::kServerUnreachable,
         "Failed to connect"},
        {"/cache/aa/debuginfo", SymbolPathError::kOk,
         "downloaded from http://up.example"}}});
  result.failed_mappings.push_back(
      {"/usr/lib/libb.so",
       "b",
       {{"/usr/lib/debug", SymbolPathError::kBuildIdNotInIndex, {}},
        {"http://up.example", SymbolPathError::kNotOnServer, {}},
        {"http://down.example", SymbolPathError::kServerUnreachable,
         "Failed to connect"},
        {"http://bad.example", SymbolPathError::kParseError,
         "invalid debug file"},
        {"http://flaky.example", SymbolPathError::kDownloadFailed,
         "The requested URL returned error: 503"}},
       3,
       0});

  std::string summary = FormatSymbolizationSummary(result, false, false);
  EXPECT_THAT(summary, testing::HasSubstr("2 of 5 frames symbolized"));
  EXPECT_THAT(summary, testing::HasSubstr("  Debuginfod: 1 downloaded, "
                                          "1 not found on any server, "
                                          "1 failed\n"));
  EXPECT_THAT(summary, testing::HasSubstr("no usable symbols in the searched "
                                          "paths or servers"));
  EXPECT_THAT(summary, testing::HasSubstr("hint: debuginfod server "
                                          "http://down.example could not be "
                                          "reached"));
  EXPECT_THAT(summary, testing::HasSubstr("hint: debuginfod: invalid debug "
                                          "file (http://bad.example)\n"));
  EXPECT_THAT(summary, testing::HasSubstr("hint: debuginfod: The requested "
                                          "URL returned error: 503 "
                                          "(http://flaky.example)\n"));
  EXPECT_THAT(summary, testing::Not(testing::HasSubstr("--debuginfod")));

  std::string verbose = FormatSymbolizationSummary(result, true, false);
  EXPECT_THAT(verbose, testing::HasSubstr("symbols: /cache/aa/debuginfo "
                                          "(2 frames, downloaded from "
                                          "http://up.example)"));
  EXPECT_THAT(verbose, testing::HasSubstr("      also tried:\n        "
                                          "http://down.example (server "
                                          "unreachable: Failed to connect)"));
  EXPECT_THAT(verbose, testing::HasSubstr("http://up.example (not on server)"));
  EXPECT_THAT(verbose, testing::HasSubstr("http://down.example (server "
                                          "unreachable: Failed to connect)"));

  result.debuginfod.enabled = false;
  EXPECT_THAT(FormatSymbolizationSummary(result, false, false),
              testing::HasSubstr("pass --debuginfod"));
}
}  // namespace
}  // namespace perfetto::profiling
