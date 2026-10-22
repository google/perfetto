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

#include "src/tracing/service/trace_buffer_factory.h"

#include "perfetto/tracing/core/trace_config.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

TEST(TraceBufferFactoryTest, CreatesRequestedImplementation) {
  TraceBufferFactory factory;
  const TraceBuffer::BufType types[] = {
      TraceBuffer::kV1,
      TraceBuffer::kV2,
  };
  for (auto type : types) {
    for (auto policy : {TraceBuffer::kOverwrite, TraceBuffer::kDiscard}) {
      TraceConfig::BufferConfig config;
      config.set_size_kb(128);
      config.set_fill_policy(policy == TraceBuffer::kDiscard
                                 ? TraceConfig::BufferConfig::DISCARD
                                 : TraceConfig::BufferConfig::RING_BUFFER);
      if (type == TraceBuffer::kV2) {
        config.mutable_buffer_type()->mutable_v2();
      } else {
        config.mutable_buffer_type()->mutable_v1();
      }

      auto buffer = factory.Create(config);
      ASSERT_TRUE(buffer.ok()) << buffer.status().message();
      ASSERT_NE(buffer->get(), nullptr);
      EXPECT_EQ((*buffer)->buf_type(), type);
      EXPECT_EQ((*buffer)->size(), 128 * 1024u);
      EXPECT_EQ((*buffer)->overwrite_policy(), policy);
      EXPECT_FALSE((*buffer)->has_data());
    }
  }
}

}  // namespace
}  // namespace perfetto
