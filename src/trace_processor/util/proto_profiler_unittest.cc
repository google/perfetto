/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "test/gtest_and_gmock.h"

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/chrome_mojo_event_info.pbzero.h"
#include "src/protozero/test/example_proto/test_messages.pbzero.h"
#include "src/trace_processor/test_messages.descriptor.h"
#include "src/trace_processor/util/proto_profiler.h"

namespace perfetto {
namespace trace_processor {
namespace util {
namespace {

using ::testing::UnorderedElementsAreArray;

TEST(ProtoProfiler, TestMessage) {
  protozero::HeapBuffered<protozero::test::protos::pbzero::NestedA> message;
  message->add_repeated_a()->set_value_b()->set_value_c(1);
  message->add_repeated_a()->set_value_b()->set_value_c(2);
  message->set_super_nested()->set_value_c(3);
  const std::vector<uint8_t> bytes = message.SerializeAsArray();

  DescriptorPool pool;
  pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                kTestMessagesDescriptor.size());
  SizeProfileComputer computer(&pool);
  const auto got_map = computer.Compute(bytes.data(), bytes.size(),
                                        ".protozero.test.protos.NestedA");

  // base::FlatHashMap doesn't support STL-container style iteration, so test
  // matchers don't work for it, and we need a vector (std::map would work,
  // too).
  using Item = std::pair<SizeProfileComputer::FieldPath,
                         SizeProfileComputer::SizeSamples>;
  std::vector<Item> got;
  for (auto it = got_map.GetIterator(); it; ++it) {
    got.emplace_back(it.key(), it.value());
  }
  std::vector<Item> expected{
      {{"NestedA"}, {15}},
      {{"NestedA", "#repeated_a", "NestedB"}, {5, 5}},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC"}, {1, 1}},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC", "#value_c",
        "int32"},
       {1, 1}},
      {{"NestedA", "#super_nested", "NestedC"}, {1}},
      {{"NestedA", "#super_nested", "NestedC", "#value_c", "int32"}, {1}}};

  EXPECT_THAT(got, UnorderedElementsAreArray(expected));
}

}  // namespace
}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto
