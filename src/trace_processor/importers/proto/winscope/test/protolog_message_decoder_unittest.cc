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

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope {

class ProtologMessageDecoderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    context_ = std::make_unique<TraceProcessorContext>();
    context_->storage = std::make_unique<TraceStorage>();
    decoder_ = std::make_unique<ProtoLogMessageDecoder>(context_.get());
    decoder_->TrackGroup(default_group_id, default_tag);
  }

  static constexpr uint32_t default_group_id = 1;
  static constexpr const char* default_tag = "DEFAULT_TAG";

  std::unique_ptr<TraceProcessorContext> context_;
  std::unique_ptr<ProtoLogMessageDecoder> decoder_;
};

TEST_F(ProtologMessageDecoderTest, DecodeSingleMessage) {
  uint64_t msg_id = 100;
  decoder_->TrackMessage(msg_id, ProtoLogLevel::INFO, default_group_id, "Test %d %s", "Some Location");

  auto decoded = decoder_->Decode(msg_id, {42}, {}, {}, {"hello"});
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->log_level, ProtoLogLevel::INFO);
  EXPECT_EQ(decoded->group_tag, default_tag);
  EXPECT_EQ(decoded->message, "Test 42 hello");
  EXPECT_EQ(decoded->location, "Some Location");
  EXPECT_EQ(context_->storage->GetStats(stats::winscope_protolog_view_config_collision), 0);
}

TEST_F(ProtologMessageDecoderTest, DecodeCollidingMessages) {
  uint64_t msg_id = 100;
  decoder_->TrackMessage(msg_id, ProtoLogLevel::INFO, default_group_id, "Value: %d", "Some Location");
  decoder_->TrackMessage(msg_id, ProtoLogLevel::WARN, default_group_id, "Name: %s", "Other Location");

  auto decoded = decoder_->Decode(msg_id, {123}, {}, {}, {});
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->log_level, ProtoLogLevel::INFO);
  EXPECT_EQ(decoded->group_tag, default_tag);
  EXPECT_THAT(decoded->message, testing::HasSubstr("<PROTOLOG COLLISION (id=0x"));
  EXPECT_THAT(decoded->message, testing::HasSubstr(") RESOLVED: 'Value: 123'>"));
  EXPECT_EQ(decoded->location, "Some Location");
  EXPECT_EQ(context_->storage->GetStats(stats::winscope_protolog_view_config_collision), 1);
  EXPECT_EQ(context_->storage->GetStats(stats::winscope_protolog_view_config_collision_resolved), 1);

  /* WIP: TESTS TO BE ADDED TOMORROW*/
}
}  // namespace perfetto::trace_processor::winscope
