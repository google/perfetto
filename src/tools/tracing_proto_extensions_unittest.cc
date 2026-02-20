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

#include "src/tools/tracing_proto_extensions.h"

#include "perfetto/base/status.h"
#include "src/base/test/tmp_dir_tree.h"
#include "src/base/test/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace gen_proto_extensions {
namespace {

TEST(GenProtoExtensionsTest, ParseRegistryBasic) {
  const char kJson[] = R"({
    "scope": "perfetto.protos.TrackEvent",
    "range": [1000, 2000],
    "allocations": [
      {
        "name": "project_a",
        "range": [1000, 1499],
        "contact": "foo@example.com",
        "description": "Project A",
        "proto": "path/to/a.proto"
      },
      {
        "name": "unallocated",
        "range": [1500, 2000]
      }
    ]
  })";

  auto result = ParseRegistry(kJson, "test.json");
  ASSERT_TRUE(result.ok()) << result.status().message();

  const Registry& reg = *result;
  EXPECT_EQ(reg.scope, "perfetto.protos.TrackEvent");
  ASSERT_EQ(reg.ranges.size(), 1u);
  EXPECT_EQ(reg.ranges[0], Range(1000, 2000));
  ASSERT_EQ(reg.allocations.size(), 2u);

  EXPECT_EQ(reg.allocations[0].name, "project_a");
  ASSERT_EQ(reg.allocations[0].ranges.size(), 1u);
  EXPECT_EQ(reg.allocations[0].ranges[0], Range(1000, 1499));
  EXPECT_EQ(reg.allocations[0].contact, "foo@example.com");
  EXPECT_EQ(reg.allocations[0].proto, "path/to/a.proto");

  EXPECT_EQ(reg.allocations[1].name, "unallocated");
  ASSERT_EQ(reg.allocations[1].ranges.size(), 1u);
  EXPECT_EQ(reg.allocations[1].ranges[0], Range(1500, 2000));
}

TEST(GenProtoExtensionsTest, ParseRegistryWithSubRegistry) {
  const char kJson[] = R"({
    "range": [1000, 2000],
    "allocations": [
      {
        "name": "project_a",
        "range": [1000, 1499],
        "registry": "path/to/sub.json"
      },
      {
        "name": "project_b",
        "range": [1500, 1999],
        "repo": "https://example.com/repo",
        "proto": "some/path.proto"
      },
      {
        "name": "unallocated",
        "range": [2000, 2000]
      }
    ]
  })";

  auto result = ParseRegistry(kJson, "test.json");
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(result->allocations[0].registry, "path/to/sub.json");
  EXPECT_EQ(result->allocations[1].repo, "https://example.com/repo");
}

TEST(GenProtoExtensionsTest, ValidateRegistryValid) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 299}};
  reg.allocations.push_back({"a", {{100, 199}}, "", "", "", "a.proto", ""});
  reg.allocations.push_back({"unallocated", {{200, 299}}, "", "", "", "", ""});

  EXPECT_TRUE(ValidateRegistry(reg).ok());
}

TEST(GenProtoExtensionsTest, ValidateRegistryGap) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 299}};
  // Gap between 150 and 200.
  reg.allocations.push_back({"a", {{100, 149}}, "", "", "", "a.proto", ""});
  reg.allocations.push_back({"unallocated", {{200, 299}}, "", "", "", "", ""});

  auto status = ValidateRegistry(reg);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("gap or overlap"));
}

TEST(GenProtoExtensionsTest, ValidateRegistryOverflow) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 199}};
  // Allocation extends beyond registry range.
  reg.allocations.push_back({"a", {{100, 250}}, "", "", "", "a.proto", ""});

  auto status = ValidateRegistry(reg);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("gap or overlap"));
}

TEST(GenProtoExtensionsTest, ValidateRegistryBothProtoAndRegistry) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 199}};
  reg.allocations.push_back(
      {"a", {{100, 199}}, "", "", "", "a.proto", "a.json"});

  auto status = ValidateRegistry(reg);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("both"));
}

TEST(GenProtoExtensionsTest, ValidateRegistryMissingProtoOrRegistry) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 199}};
  // Non-unallocated entry with neither proto nor registry nor repo.
  reg.allocations.push_back({"a", {{100, 199}}, "", "", "", "", ""});

  auto status = ValidateRegistry(reg);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("must have"));
}

TEST(GenProtoExtensionsTest, ValidateRegistryUnallocatedWithProto) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 199}};
  reg.allocations.push_back(
      {"unallocated", {{100, 199}}, "", "", "", "bad.proto", ""});

  auto status = ValidateRegistry(reg);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("Unallocated"));
}

TEST(GenProtoExtensionsTest, ParseRegistryInvalidJson) {
  auto result = ParseRegistry("{invalid", "test.json");
  EXPECT_FALSE(result.ok());
}

TEST(GenProtoExtensionsTest, ParseRegistryMissingRange) {
  const char kJson[] = R"({
    "allocations": [
      {"name": "a", "range": [1, 10], "proto": "a.proto"}
    ]
  })";
  auto result = ParseRegistry(kJson, "test.json");
  ASSERT_TRUE(result.ok());
  // ranges will be empty (no top-level range specified).
  // ValidateRegistry should catch this.
  auto status = ValidateRegistry(*result);
  EXPECT_FALSE(status.ok());
}

TEST(GenProtoExtensionsTest, ValidateRegistryRemoteEntrySkipsProtoCheck) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 299}};
  // Remote entry: has repo but no local proto or registry - should be fine.
  reg.allocations.push_back(
      {"remote", {{100, 199}}, "", "", "https://example.com", "ext.proto", ""});
  reg.allocations.push_back({"unallocated", {{200, 299}}, "", "", "", "", ""});

  EXPECT_TRUE(ValidateRegistry(reg).ok());
}

TEST(GenProtoExtensionsTest, ParseRegistryWithRanges) {
  const char kJson[] = R"({
    "ranges": [[1000, 1499], [2000, 2999]],
    "allocations": [
      {
        "name": "project_a",
        "range": [1000, 1499],
        "proto": "a.proto"
      },
      {
        "name": "project_b",
        "range": [2000, 2999],
        "proto": "b.proto"
      }
    ]
  })";

  auto result = ParseRegistry(kJson, "test.json");
  ASSERT_TRUE(result.ok()) << result.status().message();

  ASSERT_EQ(result->ranges.size(), 2u);
  EXPECT_EQ(result->ranges[0], Range(1000, 1499));
  EXPECT_EQ(result->ranges[1], Range(2000, 2999));
}

TEST(GenProtoExtensionsTest, ParseRegistryAllocWithRanges) {
  const char kJson[] = R"({
    "range": [1000, 2999],
    "allocations": [
      {
        "name": "project_a",
        "ranges": [[1000, 1499], [2000, 2499]],
        "proto": "a.proto"
      },
      {
        "name": "unallocated",
        "ranges": [[1500, 1999], [2500, 2999]]
      }
    ]
  })";

  auto result = ParseRegistry(kJson, "test.json");
  ASSERT_TRUE(result.ok()) << result.status().message();

  ASSERT_EQ(result->allocations[0].ranges.size(), 2u);
  EXPECT_EQ(result->allocations[0].ranges[0], Range(1000, 1499));
  EXPECT_EQ(result->allocations[0].ranges[1], Range(2000, 2499));
}

TEST(GenProtoExtensionsTest, ParseRegistryRangeAndRangesMutuallyExclusive) {
  const char kJson[] = R"({
    "range": [1000, 2000],
    "ranges": [[1000, 1500], [1501, 2000]],
    "allocations": []
  })";

  auto result = ParseRegistry(kJson, "test.json");
  EXPECT_FALSE(result.ok());
  EXPECT_THAT(result.status().message(), testing::HasSubstr("both"));
}

TEST(GenProtoExtensionsTest, ValidateRegistryScatteredRangesValid) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 199}, {300, 399}};
  reg.allocations.push_back(
      {"a", {{100, 199}, {300, 349}}, "", "", "", "a.proto", ""});
  reg.allocations.push_back({"unallocated", {{350, 399}}, "", "", "", "", ""});

  EXPECT_TRUE(ValidateRegistry(reg).ok());
}

TEST(GenProtoExtensionsTest, ValidateRegistryScatteredRangesGap) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 199}, {300, 399}};
  // Missing [150, 199] from the parent range - gap.
  reg.allocations.push_back(
      {"a", {{100, 149}, {300, 399}}, "", "", "", "a.proto", ""});

  auto status = ValidateRegistry(reg);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("gap or overlap"));
}

TEST(GenProtoExtensionsTest, ValidateRegistryScatteredRangesOverlap) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TrackEvent";
  reg.ranges = {{100, 399}};
  // Two allocations claim overlapping ranges.
  reg.allocations.push_back({"a", {{100, 250}}, "", "", "", "a.proto", ""});
  reg.allocations.push_back({"b", {{200, 399}}, "", "", "", "b.proto", ""});

  auto status = ValidateRegistry(reg);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("overlap"));
}

TEST(GenProtoExtensionsTest, ParseRegistryWithComment) {
  const char kJson[] = R"({
    "comment": ["This is a comment", "Another line"],
    "range": [100, 199],
    "allocations": [
      {
        "name": "a",
        "comment": ["Allocation comment"],
        "range": [100, 199],
        "proto": "a.proto"
      }
    ]
  })";

  auto result = ParseRegistry(kJson, "test.json");
  ASSERT_TRUE(result.ok()) << result.status().message();
  ASSERT_EQ(result->ranges.size(), 1u);
  EXPECT_EQ(result->ranges[0], Range(100, 199));
}

TEST(GenProtoExtensionsTest, ValidateRegistryMissingScope) {
  Registry reg;
  reg.source_path = "test.json";
  reg.ranges = {{100, 199}};
  reg.allocations.push_back({"a", {{100, 199}}, "", "", "", "a.proto", ""});

  auto status = ValidateRegistry(reg);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("scope"));
}

TEST(GenProtoExtensionsTest, ValidateRegistryWrongScope) {
  Registry reg;
  reg.source_path = "test.json";
  reg.scope = "perfetto.protos.TracePacket";
  reg.ranges = {{100, 199}};
  reg.allocations.push_back({"a", {{100, 199}}, "", "", "", "a.proto", ""});

  auto status = ValidateRegistry(reg);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.message(), testing::HasSubstr("scope"));
}

TEST(GenProtoExtensionsTest, GenerateExtensionDescriptorsNoExtend) {
  // A proto file that compiles but has no "extend TrackEvent" should be
  // rejected.
  base::TmpDirTree tmp;
  tmp.AddDir("protos");
  tmp.AddDir("protos/perfetto");
  tmp.AddDir("protos/perfetto/trace");
  tmp.AddDir("protos/perfetto/trace/track_event");
  tmp.AddFile("protos/perfetto/trace/track_event/no_extend.proto", R"(
    syntax = "proto2";
    package test;
    message Foo {
      optional int32 x = 1;
    }
  )");
  tmp.AddFile("registry.json", R"({
    "scope": "perfetto.protos.TrackEvent",
    "range": [9900, 9999],
    "allocations": [
      {
        "name": "test",
        "range": [9900, 9999],
        "proto": "protos/perfetto/trace/track_event/no_extend.proto"
      }
    ]
  })");

  auto result = GenerateExtensionDescriptors(tmp.AbsolutePath("registry.json"),
                                             {tmp.path()}, tmp.path());
  ASSERT_FALSE(result.ok());
  EXPECT_THAT(result.status().message(),
              testing::HasSubstr("no extensions targeting"));
}

TEST(GenProtoExtensionsTest, GenerateExtensionDescriptorsWithTestProto) {
  // This test uses the real test_extensions.proto from the repo.
  // It requires proto include paths to work.
  std::string proto_path = base::GetTestDataPath(
      "protos/perfetto/trace/track_event/track_event_extensions.json");
  auto result = GenerateExtensionDescriptors(proto_path, {"."}, ".");
  // This should succeed for local protos (test_extensions.proto and
  // android_track_event.proto). Remote entries (chromium) are skipped.
  ASSERT_TRUE(result.ok()) << result.status().message();
  // The output should be a non-empty FileDescriptorSet.
  EXPECT_GT(result->size(), 0u);
}

}  // namespace
}  // namespace gen_proto_extensions
}  // namespace perfetto
