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

#include "src/trace_processor/importers/proto/winscope/protolog_message_decoder.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope {

class ProtologMessageDecoderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    context_ = std::make_unique<TraceProcessorContext>();
    context_->storage = std::make_unique<TraceStorage>();
    context_->global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_->storage.get());
    context_->machine_tracker =
        std::make_unique<MachineTracker>(context_.get(), kDefaultMachineId);
    context_->trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId(0)});
    context_->stats_tracker = std::make_unique<StatsTracker>(context_.get());
    decoder_ = std::make_unique<ProtoLogMessageDecoder>(context_.get());
    decoder_->TrackGroup(default_group_id, default_tag);
  }

  static constexpr uint32_t default_group_id = 1;
  static constexpr std::string_view default_tag = "DEFAULT_TAG";

  std::unique_ptr<TraceProcessorContext> context_;
  std::unique_ptr<ProtoLogMessageDecoder> decoder_;
};

TEST_F(ProtologMessageDecoderTest, DecodeSingleMessage) {
  uint64_t msg_id = 100;
  decoder_->TrackMessage(msg_id, ProtoLogLevel::INFO, default_group_id,
                         "Test %d %s", "Some Location");

  auto decoded = decoder_->Decode(msg_id, {42}, {}, {}, {"hello"});
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->log_level, ProtoLogLevel::INFO);
  EXPECT_EQ(decoded->group_tag, default_tag);
  EXPECT_EQ(decoded->message, "Test 42 hello");
  EXPECT_EQ(decoded->location, "Some Location");
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_message_collision),
            0);
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_message_collision_resolved),
            0);
}

TEST_F(ProtologMessageDecoderTest,
       DecodeCollidingMessagesWithDifferentParameters) {
  uint64_t msg_id = 100;
  decoder_->TrackMessage(msg_id, ProtoLogLevel::INFO, default_group_id,
                         "Value: %d", "Some Location");
  decoder_->TrackMessage(msg_id, ProtoLogLevel::WARN, default_group_id,
                         "Name: %s", "Other Location");

  auto decoded = decoder_->Decode(msg_id, {123}, {}, {}, {});
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->log_level, ProtoLogLevel::INFO);
  EXPECT_EQ(decoded->group_tag, default_tag);
  EXPECT_THAT(decoded->message, "Value: 123");
  EXPECT_EQ(decoded->location, "Some Location");
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_message_collision),
            0);
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_message_collision_resolved),
            1);
}

TEST_F(ProtologMessageDecoderTest, DecodeCollidingMessagesWithSameParameters) {
  uint64_t msg_id = 100;
  decoder_->TrackMessage(msg_id, ProtoLogLevel::INFO, default_group_id,
                         "Value: %d", "Some Location");
  decoder_->TrackMessage(msg_id, ProtoLogLevel::WARN, default_group_id,
                         "Other Value: %d", "Other Location");

  auto decoded = decoder_->Decode(msg_id, {123}, {}, {}, {});
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->log_level, ProtoLogLevel::WARN);
  EXPECT_EQ(decoded->group_tag, kCollisionGroupTag);
  EXPECT_THAT(decoded->message,
              testing::HasSubstr("<PROTOLOG COLLISION (id=0x"));
  EXPECT_THAT(
      decoded->message,
      testing::HasSubstr(
          ") MULTIPLE TYPE MATCHES : 'Value: 123',\n 'Other Value: 123'>"));
  EXPECT_EQ(decoded->location, std::nullopt);
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_message_collision),
            1);
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_message_collision_resolved),
            0);
}

TEST_F(ProtologMessageDecoderTest, DecodeCollidingMessagesWithNoMatch) {
  uint64_t msg_id = 100;
  decoder_->TrackMessage(msg_id, ProtoLogLevel::INFO, default_group_id,
                         "Value: %d", "Some Location");
  decoder_->TrackMessage(msg_id, ProtoLogLevel::WARN, default_group_id,
                         "Other Value: %d", "Other Location");

  auto decoded = decoder_->Decode(msg_id, {}, {}, {true}, {});
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->log_level, ProtoLogLevel::WARN);
  EXPECT_EQ(decoded->group_tag, kCollisionGroupTag);
  EXPECT_THAT(decoded->message,
              testing::HasSubstr("<PROTOLOG COLLISION (id=0x"));
  EXPECT_THAT(decoded->message, testing::HasSubstr(") NO TYPE MATCH>"));
  EXPECT_EQ(decoded->location, std::nullopt);
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_message_collision),
            1);
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_message_collision_resolved),
            0);
}

TEST_F(ProtologMessageDecoderTest, GroupTagCollision) {
  uint64_t msg_id = 100;
  std::string_view other_group_tag = "OTHER_GROUP";
  decoder_->TrackGroup(default_group_id, other_group_tag);
  decoder_->TrackMessage(msg_id, ProtoLogLevel::INFO, default_group_id,
                         "Test %d %s", "Some Location");

  auto decoded = decoder_->Decode(msg_id, {42}, {}, {}, {"hello"});
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->log_level, ProtoLogLevel::INFO);
  EXPECT_EQ(decoded->group_tag, kCollisionGroupTag);
  EXPECT_EQ(decoded->message, "Test 42 hello");
  EXPECT_EQ(decoded->location, "Some Location");
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_group_tag_collision),
            1);
}

TEST_F(ProtologMessageDecoderTest, GroupTagMissing) {
  uint64_t msg_id = 100;
  uint32_t other_group_id = 2;
  decoder_->TrackMessage(msg_id, ProtoLogLevel::INFO, other_group_id,
                         "Test %d %s", "Some Location");

  auto decoded = decoder_->Decode(msg_id, {42}, {}, {}, {"hello"});
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->log_level, ProtoLogLevel::INFO);
  EXPECT_EQ(decoded->group_tag, kUnknownGroupTag);
  EXPECT_EQ(decoded->message, "Test 42 hello");
  EXPECT_EQ(decoded->location, "Some Location");
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_group_tag_missing),
            1);
}

TEST_F(ProtologMessageDecoderTest, MessageParameterMismatch) {
  uint64_t msg_id = 100;
  decoder_->TrackMessage(msg_id, ProtoLogLevel::INFO, default_group_id,
                         "Value: %d", "Some Location");

  auto decoded = decoder_->Decode(msg_id, {}, {}, {}, {});
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->log_level, ProtoLogLevel::INFO);
  EXPECT_EQ(decoded->group_tag, default_tag);
  EXPECT_THAT(decoded->message, "Value: [MISSING_PARAM]");
  EXPECT_EQ(decoded->location, "Some Location");
  EXPECT_EQ(context_->stats_tracker->GetStats(
                stats::winscope_protolog_param_mismatch),
            1);
}

}  // namespace perfetto::trace_processor::winscope
