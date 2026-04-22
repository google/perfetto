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

#include "perfetto/ext/traceconv/traceconv.h"

#include <unistd.h>

#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/base/test/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::traceconv {
namespace {

// Helper: builds an argv from owned strings and invokes TraceconvMain.
class ArgvInvoker {
 public:
  void Add(const std::string& arg) { args_.push_back(arg); }
  int Run() {
    std::vector<char*> argv;
    for (auto& s : args_) {
      argv.push_back(s.data());
    }
    return TraceconvMain(static_cast<int>(argv.size()), argv.data());
  }

 private:
  std::vector<std::string> args_;
};

// Writes `content` to a new temp file and returns its path. The TempFile is
// kept alive by the caller to ensure the path stays valid.
base::TempFile WriteTempFile(const std::string& content) {
  auto f = base::TempFile::Create();
  PERFETTO_CHECK(base::WriteAll(f.fd(), content.data(), content.size()) ==
                 static_cast<ssize_t>(content.size()));
  return f;
}

// Returns true if |haystack| contains |needle| as a substring.
bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

class TraceconvBundleTest : public ::testing::Test {
 protected:
  void SetUp() override {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    GTEST_SKIP() << "do not run traceconv tests on Android target";
#endif
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    GTEST_SKIP() << "TarWriter is not supported on Windows";
#endif
    input_trace_ = base::GetTestDataPath(
        "test/data/heapprofd_standalone_client_example-trace");
    output_path_ = temp_dir_.path() + "/bundle.tar";
  }

  void TearDown() override { unlink(output_path_.c_str()); }

  base::TempDir temp_dir_ = base::TempDir::Create();
  std::string input_trace_;
  std::string output_path_;
};

// `bundle --proguard-map pkg=<mapping.txt>` should include a
// `deobfuscation.pb` member in the output TAR.
TEST_F(TraceconvBundleTest, BundleWithProguardMap) {
  base::TempFile mapping = WriteTempFile(
      "com.example.Foo -> a.a:\n"
      "    void bar() -> b\n");

  ArgvInvoker invoker;
  invoker.Add("traceconv");
  invoker.Add("bundle");
  invoker.Add("--no-auto-symbol-paths");
  invoker.Add("--proguard-map");
  invoker.Add("com.example=" + mapping.path());
  invoker.Add(input_trace_);
  invoker.Add(output_path_);

  ASSERT_EQ(invoker.Run(), 0);

  std::string tar_bytes;
  ASSERT_TRUE(base::ReadFile(output_path_, &tar_bytes));
  EXPECT_FALSE(tar_bytes.empty());
  // USTAR stores filenames in the 100-byte header block, so substring matching
  // is sufficient to verify TAR membership without pulling in a TAR reader.
  EXPECT_TRUE(Contains(tar_bytes, "trace.perfetto"));
  EXPECT_TRUE(Contains(tar_bytes, "deobfuscation.pb"));
}

// `--proguard-map` may be repeated; all maps should be applied.
TEST_F(TraceconvBundleTest, BundleWithRepeatedProguardMaps) {
  base::TempFile map1 = WriteTempFile("com.example.Foo -> a.a:\n");
  base::TempFile map2 = WriteTempFile("com.example.Bar -> b.b:\n");

  ArgvInvoker invoker;
  invoker.Add("traceconv");
  invoker.Add("bundle");
  invoker.Add("--no-auto-symbol-paths");
  invoker.Add("--proguard-map");
  invoker.Add("com.example.one=" + map1.path());
  invoker.Add("--proguard-map");
  invoker.Add("com.example.two=" + map2.path());
  invoker.Add(input_trace_);
  invoker.Add(output_path_);

  ASSERT_EQ(invoker.Run(), 0);

  std::string tar_bytes;
  ASSERT_TRUE(base::ReadFile(output_path_, &tar_bytes));
  EXPECT_TRUE(Contains(tar_bytes, "deobfuscation.pb"));
}

// `--proguard-map` without a `pkg=` prefix is allowed (package inferred).
TEST_F(TraceconvBundleTest, BundleWithProguardMapNoPackage) {
  base::TempFile mapping = WriteTempFile("com.example.Foo -> a.a:\n");

  ArgvInvoker invoker;
  invoker.Add("traceconv");
  invoker.Add("bundle");
  invoker.Add("--no-auto-symbol-paths");
  invoker.Add("--proguard-map");
  invoker.Add(mapping.path());
  invoker.Add(input_trace_);
  invoker.Add(output_path_);

  ASSERT_EQ(invoker.Run(), 0);
}

// An explicit --proguard-map pointing at a missing file must fail (explicit
// paths must succeed per the enrichment contract).
TEST_F(TraceconvBundleTest, BundleWithMissingProguardMapFails) {
  ArgvInvoker invoker;
  invoker.Add("traceconv");
  invoker.Add("bundle");
  invoker.Add("--no-auto-symbol-paths");
  invoker.Add("--proguard-map");
  invoker.Add("com.example=/nonexistent/mapping.txt");
  invoker.Add(input_trace_);
  invoker.Add(output_path_);

  EXPECT_NE(invoker.Run(), 0);
}

// `--proguard-map` with no following argument is a usage error.
TEST_F(TraceconvBundleTest, BundleProguardMapMissingArgFails) {
  ArgvInvoker invoker;
  invoker.Add("traceconv");
  invoker.Add("bundle");
  invoker.Add("--proguard-map");

  EXPECT_NE(invoker.Run(), 0);
}

// `--no-auto-proguard-maps` disables Gradle auto-discovery but still accepts
// explicit maps via `--proguard-map`.
TEST_F(TraceconvBundleTest, BundleNoAutoProguardMapsWithExplicit) {
  base::TempFile mapping = WriteTempFile("com.example.Foo -> a.a:\n");

  ArgvInvoker invoker;
  invoker.Add("traceconv");
  invoker.Add("bundle");
  invoker.Add("--no-auto-symbol-paths");
  invoker.Add("--no-auto-proguard-maps");
  invoker.Add("--proguard-map");
  invoker.Add("com.example=" + mapping.path());
  invoker.Add(input_trace_);
  invoker.Add(output_path_);

  ASSERT_EQ(invoker.Run(), 0);

  std::string tar_bytes;
  ASSERT_TRUE(base::ReadFile(output_path_, &tar_bytes));
  EXPECT_TRUE(Contains(tar_bytes, "deobfuscation.pb"));
}

}  // namespace
}  // namespace perfetto::traceconv
