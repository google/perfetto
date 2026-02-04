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
    std::string content;
    return ExtractTarEntry(tar_path, entry_name, content);
  }

  // Helper to extract a TAR entry by name into a string
  bool ExtractTarEntry(const std::string& tar_path,
                       const std::string& entry_name,
                       std::string& content) {
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

      char size_str[13];
      memcpy(size_str, header + 124, 12);
      size_str[12] = '\0';
      size_t file_size = static_cast<size_t>(strtoul(size_str, nullptr, 8));
      size_t blocks = (file_size + kTarBlockSize - 1) / kTarBlockSize;

      if (name == entry_name) {
        // Extract the file content
        offset += kTarBlockSize;  // Skip header
        if (offset + file_size <= tar_content.size()) {
          content.assign(tar_content.data() + offset, file_size);
          return true;
        }
        return false;
      }

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

  // Verify deobfuscation.pb exists and contains the expected mappings
  std::string deobfuscation_data;
  ASSERT_TRUE(
      ExtractTarEntry(output_path, "deobfuscation.pb", deobfuscation_data));
  EXPECT_FALSE(deobfuscation_data.empty());

  // Verify the proto data contains the expected package and obfuscated names
  // We check for the presence of key strings that should be in the proto
  EXPECT_NE(deobfuscation_data.find("com.example"), std::string::npos)
      << "Expected package name 'com.example' in deobfuscation data";
  EXPECT_NE(deobfuscation_data.find("Foo"), std::string::npos)
      << "Expected original class name 'Foo' in deobfuscation data";
  EXPECT_NE(deobfuscation_data.find("bar"), std::string::npos)
      << "Expected original method name 'bar' in deobfuscation data";
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

  // Verify both mappings are in the deobfuscation data
  std::string deobfuscation_data;
  ASSERT_TRUE(
      ExtractTarEntry(output_path, "deobfuscation.pb", deobfuscation_data));
  EXPECT_FALSE(deobfuscation_data.empty());

  // Check for package 1 mappings
  EXPECT_NE(deobfuscation_data.find("com.pkg1"), std::string::npos)
      << "Expected package 'com.pkg1' in deobfuscation data";
  EXPECT_NE(deobfuscation_data.find("Foo"), std::string::npos)
      << "Expected class 'Foo' from pkg1 in deobfuscation data";

  // Check for package 2 mappings
  EXPECT_NE(deobfuscation_data.find("com.pkg2"), std::string::npos)
      << "Expected package 'com.pkg2' in deobfuscation data";
  EXPECT_NE(deobfuscation_data.find("Baz"), std::string::npos)
      << "Expected class 'Baz' from pkg2 in deobfuscation data";
}

}  // namespace
}  // namespace perfetto::trace_to_text
