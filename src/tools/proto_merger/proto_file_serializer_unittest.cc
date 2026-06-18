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

#include "src/tools/proto_merger/proto_file_serializer.h"

#include "src/tools/proto_merger/allowlist.h"
#include "src/tools/proto_merger/proto_merger.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace proto_merger {
namespace {

using testing::HasSubstr;
using testing::Not;

ProtoFile::Field MakeField(const std::string& type,
                           const std::string& name,
                           int number) {
  ProtoFile::Field field{};
  field.packageless_type = type;
  field.type = type;
  field.name = name;
  field.number = number;
  return field;
}

ProtoFile::Enum::Value MakeEnumValue(const std::string& name, int number) {
  ProtoFile::Enum::Value value{};
  value.name = name;
  value.number = number;
  return value;
}

TEST(ProtoFileSerializerTest, DeletedMessageFieldIsPreserved) {
  ProtoFile file;
  ProtoFile::Message message{};
  message.name = "Container";
  message.fields.push_back(MakeField("int32", "keep_me", 1));
  message.deleted_fields.push_back(MakeField("string", "deleted_upstream", 2));
  file.messages.push_back(message);

  std::string out = ProtoFileToDotProto(file);
  EXPECT_THAT(out, HasSubstr("int32 keep_me = 1;"));
  EXPECT_THAT(out, HasSubstr("string deleted_upstream = 2;"));
  EXPECT_THAT(out, HasSubstr("not present upstream"));
}

TEST(ProtoFileSerializerTest, DeletedOneofFieldIsPreserved) {
  ProtoFile file;
  ProtoFile::Message message{};
  message.name = "Container";

  ProtoFile::Oneof oneof{};
  oneof.name = "data";
  oneof.fields.push_back(MakeField("int32", "keep_me", 1));
  oneof.deleted_fields.push_back(MakeField("string", "deleted_upstream", 2));
  message.oneofs.push_back(oneof);
  file.messages.push_back(message);

  std::string out = ProtoFileToDotProto(file);
  EXPECT_THAT(out, HasSubstr("int32 keep_me = 1;"));
  EXPECT_THAT(out, HasSubstr("string deleted_upstream = 2;"));
  EXPECT_THAT(out, HasSubstr("not present upstream"));
}

TEST(ProtoFileSerializerTest, DeletedEnumValueIsPreserved) {
  ProtoFile file;
  ProtoFile::Enum en{};
  en.name = "Status";
  en.values.push_back(MakeEnumValue("OK", 0));
  en.deleted_values.push_back(MakeEnumValue("LEGACY_STATUS", 1));
  file.enums.push_back(en);

  std::string out = ProtoFileToDotProto(file);
  EXPECT_THAT(out, HasSubstr("OK = 0;"));
  EXPECT_THAT(out, HasSubstr("LEGACY_STATUS = 1;"));
  EXPECT_THAT(out, HasSubstr("not present upstream"));
}

// End-to-end regression test: a field deleted upstream inside a oneof (and a
// value deleted from an enum) must survive a merge + serialize round trip.
TEST(ProtoFileSerializerTest, MergeKeepsFieldsDeletedUpstream) {
  ProtoFile input;
  {
    ProtoFile::Message message{};
    message.name = "Container";

    ProtoFile::Enum en{};
    en.name = "Status";
    en.values.push_back(MakeEnumValue("OK", 0));
    en.values.push_back(MakeEnumValue("LEGACY_STATUS", 1));
    message.enums.push_back(en);

    ProtoFile::Oneof oneof{};
    oneof.name = "data";
    oneof.fields.push_back(MakeField("int32", "keep_me", 1));
    oneof.fields.push_back(MakeField("string", "deleted_upstream", 2));
    message.oneofs.push_back(oneof);

    message.fields.push_back(MakeField("int32", "plain_deleted", 3));
    input.messages.push_back(message);
  }

  // Upstream removed the "deleted_upstream" oneof field, the "LEGACY_STATUS"
  // enum value and the "plain_deleted" message field.
  ProtoFile upstream;
  {
    ProtoFile::Message message{};
    message.name = "Container";

    ProtoFile::Enum en{};
    en.name = "Status";
    en.values.push_back(MakeEnumValue("OK", 0));
    message.enums.push_back(en);

    ProtoFile::Oneof oneof{};
    oneof.name = "data";
    oneof.fields.push_back(MakeField("int32", "keep_me", 1));
    message.oneofs.push_back(oneof);

    upstream.messages.push_back(message);
  }

  ProtoFile merged;
  ASSERT_TRUE(MergeProtoFiles(input, upstream, Allowlist{}, merged).ok());

  std::string out = ProtoFileToDotProto(merged);
  EXPECT_THAT(out, HasSubstr("int32 keep_me = 1;"));
  EXPECT_THAT(out, HasSubstr("string deleted_upstream = 2;"));
  EXPECT_THAT(out, HasSubstr("LEGACY_STATUS = 1;"));
  EXPECT_THAT(out, HasSubstr("int32 plain_deleted = 3;"));
}

// Fields which exist only upstream and are not allowlisted should still be
// dropped from the merged output.
TEST(ProtoFileSerializerTest, MergeDropsNonAllowlistedUpstreamFields) {
  ProtoFile input;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("int32", "keep_me", 1));
    input.messages.push_back(message);
  }

  ProtoFile upstream;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("int32", "keep_me", 1));
    message.fields.push_back(MakeField("string", "new_upstream", 2));
    upstream.messages.push_back(message);
  }

  ProtoFile merged;
  ASSERT_TRUE(MergeProtoFiles(input, upstream, Allowlist{}, merged).ok());

  std::string out = ProtoFileToDotProto(merged);
  EXPECT_THAT(out, HasSubstr("int32 keep_me = 1;"));
  EXPECT_THAT(out, Not(HasSubstr("new_upstream")));
}

// Test that we don't repeatedly add the "not present upstream" comment to
// deleted fields when we perform a merge on an input that already has this
// comment.
TEST(ProtoFileSerializerTest, MergeDoesNotDuplicateDeletedComment) {
  ProtoFile input;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    ProtoFile::Field deleted_field = MakeField("string", "deleted_upstream", 2);
    // Simulate that the input already has the "not present upstream" comment
    // in the format generated by the serializer.
    deleted_field.leading_comments.push_back("");
    deleted_field.leading_comments.push_back(
        " The following enums/messages/fields are not present upstream");
    deleted_field.leading_comments.push_back("");
    message.fields.push_back(deleted_field);
    input.messages.push_back(message);
  }

  ProtoFile upstream;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    // Upstream has removed the field
    upstream.messages.push_back(message);
  }

  ProtoFile merged;
  ASSERT_TRUE(MergeProtoFiles(input, upstream, Allowlist{}, merged).ok());

  std::string out = ProtoFileToDotProto(merged);

  // The comment "The following enums/messages/fields are not present upstream"
  // should appear exactly once in the serialized output.
  size_t first_pos =
      out.find("The following enums/messages/fields are not present upstream");
  ASSERT_NE(first_pos, std::string::npos);
  size_t second_pos =
      out.find("The following enums/messages/fields are not present upstream",
               first_pos + 1);
  EXPECT_EQ(second_pos, std::string::npos)
      << "Comment was duplicated in output:\n"
      << out;
}

}  // namespace
}  // namespace proto_merger
}  // namespace perfetto
