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

#include "src/trace_processor/util/descriptors.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/protozero/test/example_proto/test_messages.pbzero.h"
#include "src/trace_processor/test_messages.descriptor.h"
#include "src/trace_processor/util/proto_profiler.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
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
  SizeProfileComputer computer(&pool, ".protozero.test.protos.NestedA");
  computer.Reset(bytes.data(), bytes.size());

  // Convert to vector for test matcher.
  using Item = std::pair<std::vector<std::string>, size_t>;
  std::vector<Item> got;
  for (auto sample = computer.GetNext(); sample; sample = computer.GetNext()) {
    std::vector<std::string> path;
    for (const auto& field : computer.GetPath().fields) {
      if (field.has_field_name())
        path.push_back(field.field_name());
      path.push_back(field.type_name());
    }
    got.emplace_back(path, *sample);
  }
  std::vector<Item> expected{
      {{"NestedA"}, 6},
      {{"NestedA", "#repeated_a", "NestedB"}, 2},
      {{"NestedA", "#repeated_a", "NestedB"}, 2},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC"}, 1},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC"}, 1},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC", "#value_c",
        "int32"},
       1},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC", "#value_c",
        "int32"},
       1},
      {{"NestedA", "#super_nested", "NestedC"}, 1},
      {{"NestedA", "#super_nested", "NestedC", "#value_c", "int32"}, 1}};

  EXPECT_THAT(got, UnorderedElementsAreArray(expected));
}

TEST(ProtoProfiler, TestMessageSurvivesPoolDestruction) {
  protozero::HeapBuffered<protozero::test::protos::pbzero::NestedA> message;
  message->add_repeated_a()->set_value_b()->set_value_c(1);
  message->add_repeated_a()->set_value_b()->set_value_c(2);
  message->set_super_nested()->set_value_c(3);
  const std::vector<uint8_t> bytes = message.SerializeAsArray();

  std::vector<std::pair<SizeProfileComputer::FieldPath, size_t>> samples;
  {
    DescriptorPool pool;
    pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                  kTestMessagesDescriptor.size());
    SizeProfileComputer computer(&pool, ".protozero.test.protos.NestedA");
    computer.Reset(bytes.data(), bytes.size());

    for (auto sample = computer.GetNext(); sample;
         sample = computer.GetNext()) {
      samples.push_back({computer.GetPath(), *sample});
    }
  }

  // Convert to vector for test matcher *after* pool destruction.
  using Item = std::pair<std::vector<std::string>, size_t>;
  std::vector<Item> got;
  for (const auto& [sample_path, sample_size] : samples) {
    std::vector<std::string> path;
    for (const auto& field : sample_path.fields) {
      if (field.has_field_name())
        path.push_back(field.field_name());
      path.push_back(field.type_name());
    }
    got.emplace_back(path, sample_size);
  }
  std::vector<Item> expected{
      {{"NestedA"}, 6},
      {{"NestedA", "#repeated_a", "NestedB"}, 2},
      {{"NestedA", "#repeated_a", "NestedB"}, 2},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC"}, 1},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC"}, 1},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC", "#value_c",
        "int32"},
       1},
      {{"NestedA", "#repeated_a", "NestedB", "#value_b", "NestedC", "#value_c",
        "int32"},
       1},
      {{"NestedA", "#super_nested", "NestedC"}, 1},
      {{"NestedA", "#super_nested", "NestedC", "#value_c", "int32"}, 1}};

  EXPECT_THAT(got, UnorderedElementsAreArray(expected));
}

}  // namespace
}  // namespace perfetto::trace_processor::util
