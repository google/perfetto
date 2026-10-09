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

#include "src/trace_processor/util/trace_enrichment/source_files.h"

#include <memory>
#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_processor::util {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// A temporary directory whose files are removed on destruction, as TempDir
// requires the directory to be empty when it is deleted.
class SourceTree {
 public:
  ~SourceTree() {
    for (const std::string& path : files_) {
      remove(path.c_str());
    }
  }

  std::string Write(const std::string& name, const std::string& contents) {
    std::string path = dir_.path() + "/" + name;
    base::ScopedFile fd = base::OpenFile(path, O_WRONLY | O_CREAT, 0644);
    PERFETTO_CHECK(fd);
    PERFETTO_CHECK(base::WriteAll(*fd, contents.data(), contents.size()) ==
                   static_cast<ssize_t>(contents.size()));
    files_.push_back(path);
    return path;
  }

  std::string path() const { return dir_.path(); }

 private:
  base::TempDir dir_ = base::TempDir::Create();
  std::vector<std::string> files_;
};

std::string SymbolsProtoWithFiles(const std::vector<std::string>& files) {
  protozero::HeapBuffered<protos::pbzero::Trace> trace;
  auto* module = trace->add_packet()->set_module_symbols();
  module->set_path("/lib.so");
  for (const std::string& file : files) {
    auto* address = module->add_address_symbols();
    address->set_address(0x1000);
    auto* line = address->add_lines();
    line->set_function_name("f");
    line->set_source_file_name(file);
    line->set_line_number(1);
  }
  return trace.SerializeAsString();
}

TEST(SourceFilesTest, CollectSourcePathsFromSymbolsProto) {
  auto tp = TraceProcessor::CreateInstance(Config());
  // Two serialized Trace messages back to back, with a duplicate, a relative
  // path and a symbolizer placeholder.
  std::string proto = SymbolsProtoWithFiles({"/src/b.cc", "??"}) +
                      SymbolsProtoWithFiles({"/src/a.cc", "/src/b.cc", "c.cc"});
  EXPECT_THAT(CollectSourcePaths(tp.get(), proto),
              ElementsAre("/src/a.cc", "/src/b.cc"));
}

TEST(SourceFilesTest, BundleSourceFilesEmitsPackets) {
  SourceTree dir;
  std::string a = dir.Write("a.cc", "int a() { return 1; }\n");
  std::string b = dir.Write("b.cc", "int b() { return 2; }\n");

  SourceFilesResult result = BundleSourceFiles({a, b}, SourceFilesConfig());
  EXPECT_EQ(result.bundled_count, 2u);
  EXPECT_THAT(result.unreadable, IsEmpty());
  EXPECT_THAT(result.skipped, IsEmpty());

  protos::gen::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(result.packets));
  ASSERT_EQ(trace.packet().size(), 2u);
  EXPECT_EQ(trace.packet()[0].source_file().path(), a);
  EXPECT_EQ(trace.packet()[0].source_file().contents(),
            "int a() { return 1; }\n");
  EXPECT_EQ(trace.packet()[1].source_file().path(), b);
}

TEST(SourceFilesTest, BundleSourceFilesRespectsLimits) {
  SourceTree dir;
  std::string small = dir.Write("small.cc", "x");
  std::string big = dir.Write("big.cc", std::string(100, 'x'));
  std::string missing = dir.path() + "/missing.cc";

  SourceFilesConfig config;
  config.max_file_bytes = 10;
  SourceFilesResult result = BundleSourceFiles({small, big, missing}, config);
  EXPECT_EQ(result.bundled_count, 1u);
  EXPECT_EQ(result.bundled_bytes, 1u);
  EXPECT_THAT(result.skipped, ElementsAre(big));
  EXPECT_THAT(result.unreadable, ElementsAre(missing));
}

TEST(SourceFilesTest, BundleSourceFilesAppliesPrefixMap) {
  SourceTree dir;
  std::string a = dir.Write("a.cc", "int a;\n");

  SourceFilesConfig config;
  config.prefix_maps = {{"/nonexistent/other", "/nope"},
                        {"/build/src", dir.path()}};
  SourceFilesResult result =
      BundleSourceFiles({"/build/src/a.cc", "/build/src/missing.cc"}, config);
  EXPECT_EQ(result.bundled_count, 1u);
  EXPECT_THAT(result.unreadable, ElementsAre("/build/src/missing.cc"));

  // The bundled path is the one from the debug info, not where it was read.
  protos::gen::Trace trace;
  ASSERT_TRUE(trace.ParseFromString(result.packets));
  ASSERT_EQ(trace.packet().size(), 1u);
  EXPECT_EQ(trace.packet()[0].source_file().path(), "/build/src/a.cc");
  EXPECT_EQ(trace.packet()[0].source_file().contents(), "int a;\n");
}

TEST(SourceFilesTest, BundleSourceFilesRespectsTotalBudget) {
  SourceTree dir;
  std::string a = dir.Write("a.cc", "aaaa");
  std::string b = dir.Write("b.cc", "bbbb");

  SourceFilesConfig config;
  config.max_total_bytes = 6;
  SourceFilesResult result = BundleSourceFiles({a, b}, config);
  EXPECT_EQ(result.bundled_count, 1u);
  EXPECT_THAT(result.skipped, ElementsAre(b));
}

}  // namespace
}  // namespace perfetto::trace_processor::util
