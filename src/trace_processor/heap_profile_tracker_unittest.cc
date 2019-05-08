/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/heap_profile_tracker.h"

#include "src/trace_processor/trace_processor_context.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace trace_processor {
namespace {

constexpr auto kFirstPacket = 0;
constexpr auto kFirstPacketMappingNameId = 1;
constexpr auto kFirstPacketBuildId = 2;
constexpr auto kFirstPacketFrameNameId = 3;

constexpr auto kFirstPacketMappingId = 1;
constexpr auto kFirstPacketFrameId = 1;

constexpr auto kSecondPacket = 1;
constexpr auto kSecondPacketMappingNameId = 3;
constexpr auto kSecondPacketBuildId = 2;
constexpr auto kSecondPacketFrameNameId = 1;

constexpr auto kSecondPacketFrameId = 2;
constexpr auto kSecondPacketMappingId = 2;

constexpr auto kMappingOffset = 123;
constexpr auto kMappingStart = 234;
constexpr auto kMappingEnd = 345;
constexpr auto kMappingLoadBias = 456;

static constexpr auto kFrameRelPc = 567;

using ::testing::ElementsAre;

class HeapProfileTrackerDupTest : public ::testing::Test {
 public:
  HeapProfileTrackerDupTest() {
    context.storage.reset(new TraceStorage());
    context.heap_profile_tracker.reset(new HeapProfileTracker(&context));

    mapping_name = context.storage->InternString("[mapping]");
    build = context.storage->InternString("[build id]");
    frame_name = context.storage->InternString("[frame]");
  }

 protected:
  void InsertMapping() {
    context.heap_profile_tracker->AddString(
        kFirstPacket, kFirstPacketMappingNameId, mapping_name);
    context.heap_profile_tracker->AddString(
        kSecondPacket, kSecondPacketMappingNameId, mapping_name);

    context.heap_profile_tracker->AddString(kFirstPacket, kFirstPacketBuildId,
                                            build);
    context.heap_profile_tracker->AddString(kSecondPacket, kSecondPacketBuildId,
                                            build);

    HeapProfileTracker::SourceMapping first_frame;
    first_frame.build_id = kFirstPacketBuildId;
    first_frame.offset = kMappingOffset;
    first_frame.start = kMappingStart;
    first_frame.end = kMappingEnd;
    first_frame.load_bias = kMappingLoadBias;
    first_frame.name_id = kFirstPacketMappingNameId;

    HeapProfileTracker::SourceMapping second_frame;
    second_frame.build_id = kSecondPacketBuildId;
    second_frame.offset = kMappingOffset;
    second_frame.start = kMappingStart;
    second_frame.end = kMappingEnd;
    second_frame.load_bias = kMappingLoadBias;
    second_frame.name_id = kSecondPacketMappingNameId;

    context.heap_profile_tracker->AddMapping(
        kFirstPacket, kFirstPacketMappingId, first_frame);
    context.heap_profile_tracker->AddMapping(
        kSecondPacket, kSecondPacketMappingId, second_frame);
  }

  void InsertFrame() {
    InsertMapping();
    context.heap_profile_tracker->AddString(
        kFirstPacket, kFirstPacketFrameNameId, frame_name);
    context.heap_profile_tracker->AddString(
        kSecondPacket, kSecondPacketFrameNameId, frame_name);

    HeapProfileTracker::SourceFrame first_frame;
    first_frame.name_id = kFirstPacketFrameNameId;
    first_frame.mapping_id = kFirstPacketMappingId;
    first_frame.rel_pc = kFrameRelPc;

    HeapProfileTracker::SourceFrame second_frame;
    second_frame.name_id = kSecondPacketFrameNameId;
    second_frame.mapping_id = kSecondPacketMappingId;
    second_frame.rel_pc = kFrameRelPc;

    context.heap_profile_tracker->AddFrame(kFirstPacket, kFirstPacketFrameId,
                                           first_frame);
    context.heap_profile_tracker->AddFrame(kSecondPacket, kSecondPacketFrameId,
                                           second_frame);
  }

  void InsertCallsite() {
    InsertFrame();

    HeapProfileTracker::SourceCallstack first_callsite = {kFirstPacketFrameId,
                                                          kFirstPacketFrameId};
    HeapProfileTracker::SourceCallstack second_callsite = {
        kSecondPacketFrameId, kSecondPacketFrameId};

    context.heap_profile_tracker->AddCallstack(kFirstPacket, 0, first_callsite);
    context.heap_profile_tracker->AddCallstack(kSecondPacket, 0,
                                               second_callsite);
  }

  StringId mapping_name;
  StringId build;
  StringId frame_name;
  TraceProcessorContext context;
};

// Insert the same mapping from two different packets, with different strings
// interned, and assert we only store one.
TEST_F(HeapProfileTrackerDupTest, Mapping) {
  InsertMapping();

  EXPECT_THAT(context.storage->heap_profile_mappings().build_ids(),
              ElementsAre(build));
  EXPECT_THAT(context.storage->heap_profile_mappings().offsets(),
              ElementsAre(kMappingOffset));
  EXPECT_THAT(context.storage->heap_profile_mappings().starts(),
              ElementsAre(kMappingStart));
  EXPECT_THAT(context.storage->heap_profile_mappings().ends(),
              ElementsAre(kMappingEnd));
  EXPECT_THAT(context.storage->heap_profile_mappings().load_biases(),
              ElementsAre(kMappingLoadBias));
  EXPECT_THAT(context.storage->heap_profile_mappings().names(),
              ElementsAre(mapping_name));
}

// Insert the same mapping from two different packets, with different strings
// interned, and assert we only store one.
TEST_F(HeapProfileTrackerDupTest, Frame) {
  InsertFrame();

  EXPECT_THAT(context.storage->heap_profile_frames().names(),
              ElementsAre(frame_name));
  EXPECT_THAT(context.storage->heap_profile_frames().mappings(),
              ElementsAre(0));
  EXPECT_THAT(context.storage->heap_profile_frames().rel_pcs(),
              ElementsAre(kFrameRelPc));
}

// Insert the same callstack from two different packets, assert it is only
// stored once.
TEST_F(HeapProfileTrackerDupTest, Callstack) {
  InsertCallsite();

  EXPECT_THAT(context.storage->heap_profile_callsites().frame_depths(),
              ElementsAre(0, 1));
  EXPECT_THAT(context.storage->heap_profile_callsites().parent_callsite_ids(),
              ElementsAre(-1, 0));
  EXPECT_THAT(context.storage->heap_profile_callsites().frame_ids(),
              ElementsAre(0, 0));
}

int64_t FindCallstack(const TraceStorage& storage,
                      int64_t depth,
                      int64_t parent,
                      int64_t frame_id) {
  const auto& callsites = storage.heap_profile_callsites();
  for (size_t i = 0; i < callsites.frame_depths().size(); ++i) {
    if (callsites.frame_depths()[i] == depth &&
        callsites.parent_callsite_ids()[i] == parent &&
        callsites.frame_ids()[i] == frame_id) {
      return static_cast<int64_t>(i);
    }
  }
  return -1;
}

// Insert multiple mappings, frames and callstacks and check result.
TEST(HeapProfileTrackerTest, Functional) {
  TraceProcessorContext context;
  context.storage.reset(new TraceStorage());
  context.heap_profile_tracker.reset(new HeapProfileTracker(&context));

  HeapProfileTracker* hpt = context.heap_profile_tracker.get();

  constexpr auto kPacket = 0;
  uint64_t next_string_intern_id = 1;

  const std::string build_ids[] = {"build1", "build2", "build3"};
  uint64_t build_id_ids[base::ArraySize(build_ids)];
  for (size_t i = 0; i < base::ArraySize(build_ids); ++i)
    build_id_ids[i] = next_string_intern_id++;

  const std::string mapping_names[] = {"map1", "map2", "map3"};
  uint64_t mapping_name_ids[base::ArraySize(mapping_names)];
  for (size_t i = 0; i < base::ArraySize(mapping_names); ++i)
    mapping_name_ids[i] = next_string_intern_id++;

  HeapProfileTracker::SourceMapping mappings[base::ArraySize(mapping_names)] =
      {};
  mappings[0].build_id = build_id_ids[0];
  mappings[0].offset = 1;
  mappings[0].start = 2;
  mappings[0].end = 3;
  mappings[0].load_bias = 0;
  mappings[0].name_id = mapping_name_ids[0];

  mappings[1].build_id = build_id_ids[1];
  mappings[1].offset = 1;
  mappings[1].start = 2;
  mappings[1].end = 3;
  mappings[1].load_bias = 1;
  mappings[1].name_id = mapping_name_ids[1];

  mappings[2].build_id = build_id_ids[2];
  mappings[2].offset = 1;
  mappings[2].start = 2;
  mappings[2].end = 3;
  mappings[2].load_bias = 2;
  mappings[2].name_id = mapping_name_ids[2];

  const std::string function_names[] = {"fun1", "fun2", "fun3", "fun4"};
  uint64_t function_name_ids[base::ArraySize(function_names)];
  for (size_t i = 0; i < base::ArraySize(function_names); ++i)
    function_name_ids[i] = next_string_intern_id++;

  HeapProfileTracker::SourceFrame frames[base::ArraySize(function_names)];
  frames[0].name_id = function_name_ids[0];
  frames[0].mapping_id = 0;
  frames[0].rel_pc = 123;

  frames[1].name_id = function_name_ids[1];
  frames[1].mapping_id = 0;
  frames[1].rel_pc = 123;

  frames[2].name_id = function_name_ids[2];
  frames[2].mapping_id = 1;
  frames[2].rel_pc = 123;

  frames[3].name_id = function_name_ids[3];
  frames[3].mapping_id = 2;
  frames[3].rel_pc = 123;

  HeapProfileTracker::SourceCallstack callstacks[3];
  callstacks[0] = {2, 1, 0};
  callstacks[1] = {2, 1, 0, 1, 0};
  callstacks[2] = {0, 2, 0, 1, 2};

  for (size_t i = 0; i < base::ArraySize(build_ids); ++i) {
    auto interned = context.storage->InternString(
        {build_ids[i].data(), build_ids[i].size()});
    hpt->AddString(kPacket, build_id_ids[i], interned);
  }
  for (size_t i = 0; i < base::ArraySize(mapping_names); ++i) {
    auto interned = context.storage->InternString(
        {mapping_names[i].data(), mapping_names[i].size()});
    hpt->AddString(kPacket, mapping_name_ids[i], interned);
  }
  for (size_t i = 0; i < base::ArraySize(function_names); ++i) {
    auto interned = context.storage->InternString(
        {function_names[i].data(), function_names[i].size()});
    hpt->AddString(kPacket, function_name_ids[i], interned);
  }

  for (size_t i = 0; i < base::ArraySize(mappings); ++i)
    hpt->AddMapping(kPacket, i, mappings[i]);
  for (size_t i = 0; i < base::ArraySize(frames); ++i)
    hpt->AddFrame(kPacket, i, frames[i]);
  for (size_t i = 0; i < base::ArraySize(callstacks); ++i)
    hpt->AddCallstack(kPacket, i, callstacks[i]);

  for (size_t i = 0; i < base::ArraySize(callstacks); ++i) {
    int64_t parent = -1;
    const HeapProfileTracker::SourceCallstack& callstack = callstacks[i];
    for (size_t depth = 0; depth < callstack.size(); ++depth) {
      auto frame_id =
          hpt->GetDatabaseFrameIdForTesting(kPacket, callstack[depth]);
      ASSERT_NE(frame_id, -1);
      int64_t self = FindCallstack(
          *context.storage, static_cast<int64_t>(depth), parent, frame_id);
      ASSERT_NE(self, -1);
      parent = self;
    }
  }
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
