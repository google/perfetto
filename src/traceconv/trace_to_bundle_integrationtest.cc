/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/traceconv/trace_to_bundle.h"

#include <cstring>
#include <fstream>
#include <string>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_to_text {
namespace {

class TraceToBundleTest : public ::testing::Test {
 protected:
  // Helper to check if a TAR file contains a specific entry by name
  bool TarContainsEntry(const std::string& tar_path,
                        const std::string& entry_name) {
    std::string tar_content;
    if (!base::ReadFile(tar_path, &tar_content)) {
      return false;
    }

    constexpr size_t kTarBlockSize = 512;
    for (size_t offset = 0; offset + kTarBlockSize <= tar_content.size();
         offset += kTarBlockSize) {
      const char* header = tar_content.data() + offset;

      bool is_zero_block = true;
      for (size_t i = 0; i < kTarBlockSize && is_zero_block; ++i) {
        if (header[i] != '\0') {
          is_zero_block = false;
        }
      }
      if (is_zero_block) {
        break;
      }

      std::string name(header, strnlen(header, 100));
      if (name == entry_name) {
        return true;
      }

      char size_str[13];
      memcpy(size_str, header + 124, 12);
      size_str[12] = '\0';
      size_t file_size = static_cast<size_t>(strtoul(size_str, nullptr, 8));
      size_t blocks = (file_size + kTarBlockSize - 1) / kTarBlockSize;
      offset += blocks * kTarBlockSize;
    }
    return false;
  }
};

TEST_F(TraceToBundleTest, CreatesBundleWithTrace) {
  const char* input_trace = "test/data/android_sched_and_ps.pb";

  std::ifstream test_file(input_trace);
  if (!test_file.good()) {
    GTEST_SKIP() << "Test trace file not available: " << input_trace;
  }
  test_file.close();

  base::TempFile output_file = base::TempFile::Create();
  std::string output_path = output_file.path();

  BundleContext context;
  context.no_auto_symbol_paths = true;

  int result = TraceToBundle(input_trace, output_path, context);

  EXPECT_EQ(result, 0);
  EXPECT_TRUE(TarContainsEntry(output_path, "trace.perfetto"));
}

TEST_F(TraceToBundleTest, AcceptsAndroidTrace) {
  const char* input_trace = "test/data/android_boot.pftrace";

  std::ifstream test_file(input_trace);
  if (!test_file.good()) {
    GTEST_SKIP() << "Test trace file not available: " << input_trace;
  }
  test_file.close();

  base::TempFile output_file = base::TempFile::Create();
  std::string output_path = output_file.path();

  BundleContext context;
  context.no_auto_symbol_paths = true;

  int result = TraceToBundle(input_trace, output_path, context);
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(TarContainsEntry(output_path, "trace.perfetto"));
}

TEST_F(TraceToBundleTest, AcceptsExplicitSymbolPaths) {
  const char* input_trace = "test/data/android_sched_and_ps.pb";

  std::ifstream test_file(input_trace);
  if (!test_file.good()) {
    GTEST_SKIP() << "Test trace file not available: " << input_trace;
  }
  test_file.close();

  base::TempFile output_file = base::TempFile::Create();
  std::string output_path = output_file.path();

  BundleContext context;
  context.symbol_paths = {"test/data"};
  context.no_auto_symbol_paths = true;

  int result = TraceToBundle(input_trace, output_path, context);
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(TarContainsEntry(output_path, "trace.perfetto"));
}

TEST_F(TraceToBundleTest, FailsOnInvalidInputFile) {
  base::TempFile output_file = base::TempFile::Create();
  std::string output_path = output_file.path();

  BundleContext context;
  context.no_auto_symbol_paths = true;

  int result = TraceToBundle("/nonexistent/trace.pb", output_path, context);
  EXPECT_NE(result, 0);
}

TEST_F(TraceToBundleTest, IncludesDeobfuscationData) {
  const char* input_trace = "test/data/android_sched_and_ps.pb";

  std::ifstream test_file(input_trace);
  if (!test_file.good()) {
    GTEST_SKIP() << "Test trace file not available: " << input_trace;
  }
  test_file.close();

  base::TempFile mapping_file = base::TempFile::Create();
  std::string mapping_content =
      "com.example.Foo -> a:\n"
      "    void bar() -> b\n";
  base::WriteAll(mapping_file.fd(), mapping_content.data(),
                 mapping_content.size());

  base::TempFile output_file = base::TempFile::Create();
  std::string output_path = output_file.path();

  BundleContext context;
  context.no_auto_symbol_paths = true;
  context.proguard_maps = {{"com.example", mapping_file.path()}};

  int result = TraceToBundle(input_trace, output_path, context);
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(TarContainsEntry(output_path, "trace.perfetto"));
  EXPECT_TRUE(TarContainsEntry(output_path, "deobfuscation.pb"));
}

TEST_F(TraceToBundleTest, NoDeobfuscationWithoutMaps) {
  const char* input_trace = "test/data/android_sched_and_ps.pb";

  std::ifstream test_file(input_trace);
  if (!test_file.good()) {
    GTEST_SKIP() << "Test trace file not available: " << input_trace;
  }
  test_file.close();

  base::TempFile output_file = base::TempFile::Create();
  std::string output_path = output_file.path();

  BundleContext context;
  context.no_auto_symbol_paths = true;

  int result = TraceToBundle(input_trace, output_path, context);
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(TarContainsEntry(output_path, "trace.perfetto"));
  EXPECT_FALSE(TarContainsEntry(output_path, "deobfuscation.pb"));
}

TEST_F(TraceToBundleTest, FailsOnNonexistentProguardMap) {
  const char* input_trace = "test/data/android_sched_and_ps.pb";

  std::ifstream test_file(input_trace);
  if (!test_file.good()) {
    GTEST_SKIP() << "Test trace file not available: " << input_trace;
  }
  test_file.close();

  base::TempFile output_file = base::TempFile::Create();
  std::string output_path = output_file.path();

  BundleContext context;
  context.no_auto_symbol_paths = true;
  context.proguard_maps = {{"com.example", "/nonexistent/mapping.txt"}};

  int result = TraceToBundle(input_trace, output_path, context);
  EXPECT_NE(result, 0);
}

TEST_F(TraceToBundleTest, HandlesMultipleProguardMaps) {
  const char* input_trace = "test/data/android_sched_and_ps.pb";

  std::ifstream test_file(input_trace);
  if (!test_file.good()) {
    GTEST_SKIP() << "Test trace file not available: " << input_trace;
  }
  test_file.close();

  base::TempFile mapping_file1 = base::TempFile::Create();
  std::string content1 = "com.pkg1.Foo -> a:\n    void bar() -> b\n";
  base::WriteAll(mapping_file1.fd(), content1.data(), content1.size());

  base::TempFile mapping_file2 = base::TempFile::Create();
  std::string content2 = "com.pkg2.Baz -> c:\n    int qux() -> d\n";
  base::WriteAll(mapping_file2.fd(), content2.data(), content2.size());

  base::TempFile output_file = base::TempFile::Create();
  std::string output_path = output_file.path();

  BundleContext context;
  context.no_auto_symbol_paths = true;
  context.proguard_maps = {{"com.pkg1", mapping_file1.path()},
                           {"com.pkg2", mapping_file2.path()}};

  int result = TraceToBundle(input_trace, output_path, context);
  EXPECT_EQ(result, 0);
  EXPECT_TRUE(TarContainsEntry(output_path, "deobfuscation.pb"));
}

}  // namespace
}  // namespace perfetto::trace_to_text
