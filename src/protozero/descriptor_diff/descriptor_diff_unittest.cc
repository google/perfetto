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

#include "src/protozero/descriptor_diff/descriptor_diff.h"

#include <string>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "test/gtest_and_gmock.h"

namespace protozero {
namespace {

using ::perfetto::base::StatusOr;
using ::perfetto::protos::pbzero::FileDescriptorProto;
using ::perfetto::protos::pbzero::FileDescriptorSet;
using ::protozero::HeapBuffered;

TEST(DescriptorDiff, EmptyBoth) {
  StatusOr<std::string> out = DescriptorDiff("", "");

  ASSERT_TRUE(out.ok());
  EXPECT_TRUE(out->empty());
}

TEST(DescriptorDiff, EmptySubtrahend) {
  HeapBuffered<FileDescriptorSet> minuend;
  minuend->add_file()->set_name("foo.proto");
  minuend->add_file()->set_name("bar.proto");

  std::string serialized_minuend = minuend.SerializeAsString();

  StatusOr<std::string> out = DescriptorDiff(serialized_minuend, "");

  ASSERT_TRUE(out.ok());
  EXPECT_EQ(*out, serialized_minuend);
}

TEST(DescriptorDiff, EmptyMinuend) {
  HeapBuffered<FileDescriptorSet> subtrahend;
  subtrahend->add_file()->set_name("foo.proto");
  subtrahend->add_file()->set_name("bar.proto");

  StatusOr<std::string> out =
      DescriptorDiff("", subtrahend.SerializeAsString());

  ASSERT_TRUE(out.ok());
  EXPECT_TRUE(out->empty());
}

TEST(DescriptorDiff, InvalidMinuend) {
  HeapBuffered<FileDescriptorSet> minuend;
  // KFileFieldNumber is not VarInt, it's a nested message.
  minuend->AppendVarInt(FileDescriptorSet::kFileFieldNumber, 5);

  StatusOr<std::string> out = DescriptorDiff(minuend.SerializeAsString(), "");

  EXPECT_FALSE(out.ok());
}

TEST(DescriptorDiff, InvalidSubtrahend) {
  HeapBuffered<FileDescriptorSet> subtrahend;
  // KFileFieldNumber is not VarInt, it's a nested message.
  subtrahend->AppendVarInt(FileDescriptorSet::kFileFieldNumber, 5);

  StatusOr<std::string> out =
      DescriptorDiff("", subtrahend.SerializeAsString());

  EXPECT_FALSE(out.ok());
}

TEST(DescriptorDiff, UnknownFileDescriptorSetField) {
  HeapBuffered<FileDescriptorSet> msg;
  static_assert(5 != FileDescriptorSet::kFileFieldNumber);
  msg->AppendString(5, "FieldContent");

  StatusOr<std::string> out =
      DescriptorDiff(msg.SerializeAsString(), msg.SerializeAsString());

  ASSERT_TRUE(out.ok());
  ProtoDecoder set(*out);
  Field field = set.ReadField();
  ASSERT_TRUE(field.valid());
  ASSERT_EQ(field.type(), proto_utils::ProtoWireType::kLengthDelimited);
  EXPECT_EQ(field.as_std_string(), "FieldContent");
  field = set.ReadField();
  EXPECT_FALSE(field.valid());
}

TEST(DescriptorDiff, Equal) {
  HeapBuffered<FileDescriptorSet> msg;
  msg->add_file()->set_name("foo.proto");
  msg->add_file()->set_name("bar.proto");

  StatusOr<std::string> out =
      DescriptorDiff(msg.SerializeAsString(), msg.SerializeAsString());

  ASSERT_TRUE(out.ok());
  EXPECT_TRUE(out->empty());
}

TEST(DescriptorDiff, All) {
  HeapBuffered<FileDescriptorSet> minuend;
  {
    {
      auto* file = minuend->add_file();
      file->set_name("foo.proto");
      file->set_package("package_foo");
    }
    {
      auto* file = minuend->add_file();
      file->set_name("bar.proto");
      file->set_package("package_bar");
    }
  }

  HeapBuffered<FileDescriptorSet> subtrahend;
  subtrahend->add_file()->set_name("bar.proto");
  subtrahend->add_file()->set_name("baz.proto");

  StatusOr<std::string> out = DescriptorDiff(minuend.SerializeAsString(),
                                             subtrahend.SerializeAsString());

  ASSERT_TRUE(out.ok());
  FileDescriptorSet::Decoder set(reinterpret_cast<const uint8_t*>(out->data()),
                                 out->size());
  auto it = set.file();
  ASSERT_TRUE(it);
  FileDescriptorProto::Decoder file(it->data(), it->size());
  EXPECT_EQ(file.name().ToStdStringView(), "foo.proto");
  EXPECT_EQ(file.package().ToStdStringView(), "package_foo");
  it++;
  EXPECT_FALSE(it);
}

}  // namespace
}  // namespace protozero
