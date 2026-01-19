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

#include "src/trace_processor/util/symbol_path_discovery.h"

#include "src/base/test/tmp_dir_tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {

using ::testing::Contains;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;

class SymbolPathDiscoveryTest : public ::testing::Test {
 protected:
  base::TmpDirTree tmp_;
};

TEST_F(SymbolPathDiscoveryTest, EmptyInputsReturnsEmpty) {
  auto result = DiscoverSymbolPaths({}, "", "");

  EXPECT_THAT(result.native_symbol_paths, IsEmpty());
  EXPECT_THAT(result.proguard_map_paths, IsEmpty());
}

TEST_F(SymbolPathDiscoveryTest, ExplicitPathsPreserved) {
  auto result = DiscoverSymbolPaths({"/explicit/path1", "/explicit/path2"},
                                    "",  // no android_product_out
                                    ""   // no working_dir
  );

  // Explicit paths are always preserved, even if they don't exist
  EXPECT_THAT(result.native_symbol_paths, SizeIs(2));
  EXPECT_THAT(result.native_symbol_paths, Contains("/explicit/path1"));
  EXPECT_THAT(result.native_symbol_paths, Contains("/explicit/path2"));
}

TEST_F(SymbolPathDiscoveryTest, ExplicitEmptyPathsFiltered) {
  auto result = DiscoverSymbolPaths({"", "/valid/path", ""}, "", "");

  // Empty strings should be filtered out
  EXPECT_THAT(result.native_symbol_paths, SizeIs(1));
  EXPECT_THAT(result.native_symbol_paths, Contains("/valid/path"));
}

TEST_F(SymbolPathDiscoveryTest, AndroidProductOutSymbolsFound) {
  // Create the symbols directory
  tmp_.AddDir("symbols");

  auto result = DiscoverSymbolPaths({},
                                    tmp_.path(),  // android_product_out
                                    ""            // no working_dir
  );

  std::string expected = tmp_.path() + "/symbols";
  EXPECT_THAT(result.native_symbol_paths, Contains(expected));
}

TEST_F(SymbolPathDiscoveryTest, AndroidProductOutSymbolsMissing) {
  // Don't create the symbols directory
  auto result = DiscoverSymbolPaths({},
                                    tmp_.path(),  // android_product_out
                                    ""            // no working_dir
  );

  // Should not add non-existent path
  std::string not_expected = tmp_.path() + "/symbols";
  EXPECT_THAT(result.native_symbol_paths, Not(Contains(not_expected)));
}

TEST_F(SymbolPathDiscoveryTest, GradleCmakePathFound) {
  tmp_.AddDir("app");
  tmp_.AddDir("app/build");
  tmp_.AddDir("app/build/intermediates");
  tmp_.AddDir("app/build/intermediates/cmake");

  auto result = DiscoverSymbolPaths({},
                                    "",           // no android_product_out
                                    tmp_.path()   // working_dir
  );

  std::string expected = tmp_.path() + "/app/build/intermediates/cmake";
  EXPECT_THAT(result.native_symbol_paths, Contains(expected));
}

TEST_F(SymbolPathDiscoveryTest, GradleMergedNativeLibsFound) {
  tmp_.AddDir("app");
  tmp_.AddDir("app/build");
  tmp_.AddDir("app/build/intermediates");
  tmp_.AddDir("app/build/intermediates/merged_native_libs");

  auto result = DiscoverSymbolPaths({},
                                    "",           // no android_product_out
                                    tmp_.path()   // working_dir
  );

  std::string expected =
      tmp_.path() + "/app/build/intermediates/merged_native_libs";
  EXPECT_THAT(result.native_symbol_paths, Contains(expected));
}

TEST_F(SymbolPathDiscoveryTest, GradleBuildIdCacheFound) {
  tmp_.AddDir(".build-id");

  auto result = DiscoverSymbolPaths({},
                                    "",           // no android_product_out
                                    tmp_.path()   // working_dir
  );

  std::string expected = tmp_.path() + "/.build-id";
  EXPECT_THAT(result.native_symbol_paths, Contains(expected));
}

TEST_F(SymbolPathDiscoveryTest, GradleProguardMappingReleaseFound) {
  tmp_.AddDir("app");
  tmp_.AddDir("app/build");
  tmp_.AddDir("app/build/outputs");
  tmp_.AddDir("app/build/outputs/mapping");
  tmp_.AddDir("app/build/outputs/mapping/release");
  tmp_.AddFile("app/build/outputs/mapping/release/mapping.txt",
               "com.example.Foo -> a:\n");

  auto result = DiscoverSymbolPaths({},
                                    "",           // no android_product_out
                                    tmp_.path()   // working_dir
  );

  std::string expected =
      tmp_.path() + "/app/build/outputs/mapping/release/mapping.txt";
  EXPECT_THAT(result.proguard_map_paths, Contains(expected));
}

TEST_F(SymbolPathDiscoveryTest, GradleProguardMappingDebugFound) {
  tmp_.AddDir("app");
  tmp_.AddDir("app/build");
  tmp_.AddDir("app/build/outputs");
  tmp_.AddDir("app/build/outputs/mapping");
  tmp_.AddDir("app/build/outputs/mapping/debug");
  tmp_.AddFile("app/build/outputs/mapping/debug/mapping.txt",
               "com.example.Bar -> b:\n");

  auto result = DiscoverSymbolPaths({},
                                    "",           // no android_product_out
                                    tmp_.path()   // working_dir
  );

  std::string expected =
      tmp_.path() + "/app/build/outputs/mapping/debug/mapping.txt";
  EXPECT_THAT(result.proguard_map_paths, Contains(expected));
}

TEST_F(SymbolPathDiscoveryTest, GradleProguardMappingBothVariants) {
  tmp_.AddDir("app");
  tmp_.AddDir("app/build");
  tmp_.AddDir("app/build/outputs");
  tmp_.AddDir("app/build/outputs/mapping");
  tmp_.AddDir("app/build/outputs/mapping/release");
  tmp_.AddDir("app/build/outputs/mapping/debug");
  tmp_.AddFile("app/build/outputs/mapping/release/mapping.txt", "release\n");
  tmp_.AddFile("app/build/outputs/mapping/debug/mapping.txt", "debug\n");

  auto result = DiscoverSymbolPaths({},
                                    "",           // no android_product_out
                                    tmp_.path()   // working_dir
  );

  EXPECT_THAT(result.proguard_map_paths, SizeIs(2));
}

TEST_F(SymbolPathDiscoveryTest, GradleProguardMappingDirExistsNoFile) {
  // Create directory but no mapping.txt file
  tmp_.AddDir("app");
  tmp_.AddDir("app/build");
  tmp_.AddDir("app/build/outputs");
  tmp_.AddDir("app/build/outputs/mapping");
  tmp_.AddDir("app/build/outputs/mapping/release");

  auto result = DiscoverSymbolPaths({},
                                    "",           // no android_product_out
                                    tmp_.path()   // working_dir
  );

  // Should not find any proguard maps
  EXPECT_THAT(result.proguard_map_paths, IsEmpty());
}

TEST_F(SymbolPathDiscoveryTest, AllPathsDiscoveredTogether) {
  // Set up all possible paths
  tmp_.AddDir("symbols");
  tmp_.AddDir("app");
  tmp_.AddDir("app/build");
  tmp_.AddDir("app/build/intermediates");
  tmp_.AddDir("app/build/intermediates/cmake");
  tmp_.AddDir("app/build/intermediates/merged_native_libs");
  tmp_.AddDir(".build-id");
  tmp_.AddDir("app/build/outputs");
  tmp_.AddDir("app/build/outputs/mapping");
  tmp_.AddDir("app/build/outputs/mapping/release");
  tmp_.AddFile("app/build/outputs/mapping/release/mapping.txt", "test\n");

  auto result = DiscoverSymbolPaths({"/explicit/path"},
                                    tmp_.path(),  // android_product_out
                                    tmp_.path()   // working_dir
  );

  // Should find all paths: 1 explicit + 4 discovered (symbols, cmake,
  // merged_native_libs, .build-id)
  EXPECT_THAT(result.native_symbol_paths, SizeIs(5));
  EXPECT_THAT(result.proguard_map_paths, SizeIs(1));
}

TEST_F(SymbolPathDiscoveryTest, NonExistentWorkingDirHandledGracefully) {
  auto result = DiscoverSymbolPaths({},
                                    "",                        // no android_product_out
                                    "/nonexistent/directory"   // working_dir
  );

  // Should not crash, just return empty
  EXPECT_THAT(result.native_symbol_paths, IsEmpty());
  EXPECT_THAT(result.proguard_map_paths, IsEmpty());
}

}  // namespace
}  // namespace perfetto::trace_processor::util
