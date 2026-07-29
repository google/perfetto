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

#include <google/protobuf/compiler/importer.h>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/protozero/multifile_error_collector.h"
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

void VerifyDeletedCommentNotDuplicated(
    const std::vector<std::string>& existing_comments) {
  ProtoFile input;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    ProtoFile::Field deleted_field = MakeField("string", "deleted_upstream", 2);
    deleted_field.leading_comments = existing_comments;
    message.fields.push_back(deleted_field);
    input.messages.push_back(message);
  }

  ProtoFile upstream;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    upstream.messages.push_back(message);
  }

  ProtoFile merged;
  ASSERT_TRUE(MergeProtoFiles(input, upstream, Allowlist{}, merged).ok());

  std::string out = ProtoFileToDotProto(merged);

  size_t first_pos = out.find(kDeletedCommentWarning);
  ASSERT_NE(first_pos, std::string::npos);
  size_t second_pos = out.find(kDeletedCommentWarning, first_pos + 1);
  EXPECT_EQ(second_pos, std::string::npos)
      << "Comment was duplicated in output:\n"
      << out;
}

// Test that we don't repeatedly add the "not present upstream" comment to
// deleted fields when we perform a merge on an input that already has this
// comment.
TEST(ProtoFileSerializerTest, MergeDoesNotDuplicateDeletedComment) {
  VerifyDeletedCommentNotDuplicated(
      {"", " The following enums/messages/fields are not present upstream",
       ""});
}

TEST(ProtoFileSerializerTest, MergeDoesNotDuplicateDeletedCommentFormatted) {
  VerifyDeletedCommentNotDuplicated(
      {"The following enums/messages/fields are not present upstream"});
}

TEST(ProtoFileSerializerTest, TypeTransitionDisallowedFails) {
  ProtoFile input;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("bool", "flag", 1));
    input.messages.push_back(message);
  }

  ProtoFile upstream;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("string", "flag", 1));
    upstream.messages.push_back(message);
  }

  ProtoFile merged;
  base::Status status = MergeProtoFiles(input, upstream, Allowlist{}, merged);
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.c_message(), HasSubstr("changed from bool to string"));
}

TEST(ProtoFileSerializerTest, TypeTransitionAllowedSucceeds) {
  ProtoFile input;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("bool", "flag", 1));
    input.messages.push_back(message);
  }

  ProtoFile upstream;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("uint32", "flag", 1));
    upstream.messages.push_back(message);
  }

  ProtoFile merged;
  base::Status status = MergeProtoFiles(input, upstream, Allowlist{}, merged);
  ASSERT_TRUE(status.ok()) << status.c_message();

  std::string out = ProtoFileToDotProto(merged);
  EXPECT_THAT(out, HasSubstr("uint32 flag = 1;"));
  EXPECT_THAT(out, Not(HasSubstr("bool flag")));
}

TEST(ProtoFileSerializerTest, TypeTransitionEnumAllowedSucceeds) {
  ProtoFile input;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("bool", "state", 1));
    input.messages.push_back(message);
  }

  ProtoFile upstream;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("Container.MyEnum", "state", 1));

    ProtoFile::Enum en{};
    en.name = "MyEnum";
    en.values.push_back(MakeEnumValue("UNKNOWN", 0));
    en.values.push_back(MakeEnumValue("ACTIVE", 1));
    message.enums.push_back(en);

    upstream.messages.push_back(message);
  }

  ProtoFile merged;
  base::Status status = MergeProtoFiles(input, upstream, Allowlist{}, merged);
  ASSERT_TRUE(status.ok()) << status.c_message();

  std::string out = ProtoFileToDotProto(merged);
  EXPECT_THAT(out, HasSubstr("Container.MyEnum state = 1;"));
  EXPECT_THAT(out, Not(HasSubstr("bool state")));
}

TEST(ProtoFileSerializerTest, AllowlistedOptionIsMerged) {
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
    auto field = MakeField("int32", "keep_me", 1);
    field.options.push_back({"deprecated", "true"});
    message.fields.push_back(field);
    upstream.messages.push_back(message);
  }

  ProtoFile merged;
  ASSERT_TRUE(
      MergeProtoFiles(input, upstream, Allowlist{}, merged, {"deprecated"})
          .ok());

  std::string out = ProtoFileToDotProto(merged);
  EXPECT_THAT(out, HasSubstr("int32 keep_me = 1 [deprecated = true];"));
}

TEST(ProtoFileSerializerTest, AllowlistedOptionOnEnumIsMerged) {
  ProtoFile input;
  {
    ProtoFile::Enum en{};
    en.name = "MyEnum";
    en.values.push_back(MakeEnumValue("ACTIVE", 1));
    input.enums.push_back(en);
  }

  ProtoFile upstream;
  {
    ProtoFile::Enum en{};
    en.name = "MyEnum";
    auto val = MakeEnumValue("ACTIVE", 1);
    val.options.push_back({"deprecated", "true"});
    en.values.push_back(val);
    upstream.enums.push_back(en);
  }

  ProtoFile merged;
  ASSERT_TRUE(
      MergeProtoFiles(input, upstream, Allowlist{}, merged, {"deprecated"})
          .ok());

  std::string out = ProtoFileToDotProto(merged);
  EXPECT_THAT(out, HasSubstr("ACTIVE = 1 [deprecated = true];"));
}

TEST(ProtoFileSerializerTest,
     PassthroughFieldAutomaticallyAcceptsSubmessageFields) {
  struct ScopedUnlink {
    std::string path;
    ~ScopedUnlink() { base::Unlink(path.c_str()); }
  };

  base::TempDir temp_dir = base::TempDir::Create();
  std::string input_path = temp_dir.path() + "/input.proto";
  std::string upstream_path = temp_dir.path() + "/upstream.proto";

  ScopedUnlink unlink_input{input_path};
  ScopedUnlink unlink_upstream{upstream_path};

  std::string input_content = R"(
    syntax = "proto2";
    package perfetto.protos;
    import "protos/perfetto/common/passthrough.proto";

    message SubMessage {
      optional int32 f1 = 1;
    }

    message RootMessage {
      optional SubMessage sub = 1 [(perfetto.protos.proto_filter_merge_passthrough) = true];
    }
  )";

  std::string upstream_content = R"(
    syntax = "proto2";
    package perfetto.protos;

    message SubMessage {
      optional int32 f1 = 1;
      optional string f2 = 2; // NEW
      optional int64 f3 = 3; // NEW
    }

    message RootMessage {
      optional SubMessage sub = 1;
    }
  )";

  {
    base::ScopedFile file(base::OpenFile(input_path, O_CREAT | O_WRONLY, 0600));
    ASSERT_TRUE(file);
    ASSERT_TRUE(
        base::WriteAll(*file, input_content.c_str(), input_content.size()));
  }
  {
    base::ScopedFile file(
        base::OpenFile(upstream_path, O_CREAT | O_WRONLY, 0600));
    ASSERT_TRUE(file);
    ASSERT_TRUE(base::WriteAll(*file, upstream_content.c_str(),
                               upstream_content.size()));
  }

  protozero::MultiFileErrorCollectorImpl mfe;
  google::protobuf::compiler::DiskSourceTree dst;
  dst.MapPath("", temp_dir.path());
  dst.MapPath("", ".");                        // repo root for imports
  dst.MapPath("", "buildtools/protobuf/src");  // standard protobuf headers

  google::protobuf::compiler::Importer importer_input(&dst, &mfe);
  const auto* input_desc = importer_input.Import("input.proto");

  google::protobuf::compiler::Importer importer_upstream(&dst, &mfe);
  const auto* upstream_desc = importer_upstream.Import("upstream.proto");

  ASSERT_NE(input_desc, nullptr);
  ASSERT_NE(upstream_desc, nullptr);

  Allowlist allowed;
  ASSERT_TRUE(
      AllowlistFromPassthrough(*input_desc, *upstream_desc, allowed).ok());

  // Convert to ProtoFile and merge
  ProtoFile input_file = ProtoFileFromDescriptor("", *input_desc);
  ProtoFile upstream_file = ProtoFileFromDescriptor("", *upstream_desc);

  ProtoFile merged;
  ASSERT_TRUE(MergeProtoFiles(input_file, upstream_file, allowed, merged).ok());

  std::string out = ProtoFileToDotProto(merged);
  EXPECT_THAT(out, HasSubstr("int32 f1 = 1;"));
  EXPECT_THAT(out, HasSubstr("string f2 = 2;"));
  EXPECT_THAT(out, HasSubstr("int64 f3 = 3;"));
  EXPECT_THAT(out, HasSubstr("proto_filter_merge_passthrough) = true"));
}

TEST(ProtoFileSerializerTest, PassthroughInNestedDefinition) {
  struct ScopedUnlink {
    std::string path;
    ~ScopedUnlink() { base::Unlink(path.c_str()); }
  };

  base::TempDir temp_dir = base::TempDir::Create();
  std::string input_path = temp_dir.path() + "/input.proto";
  std::string upstream_path = temp_dir.path() + "/upstream.proto";

  ScopedUnlink unlink_input{input_path};
  ScopedUnlink unlink_upstream{upstream_path};

  std::string input_content = R"(
    syntax = "proto2";
    package perfetto.protos;
    import "protos/perfetto/common/passthrough.proto";

    message SubMessage {
      optional int32 f1 = 1;
    }

    message Outer {
      message Inner {
        optional SubMessage sub = 1 [(perfetto.protos.proto_filter_merge_passthrough) = true];
      }
      optional Inner inner = 1;
    }
  )";

  std::string upstream_content = R"(
    syntax = "proto2";
    package perfetto.protos;

    message SubMessage {
      optional int32 f1 = 1;
      optional string f2 = 2; // NEW
    }

    message Outer {
      message Inner {
        optional SubMessage sub = 1;
      }
      optional Inner inner = 1;
    }
  )";

  {
    base::ScopedFile file(base::OpenFile(input_path, O_CREAT | O_WRONLY, 0600));
    ASSERT_TRUE(file);
    ASSERT_TRUE(
        base::WriteAll(*file, input_content.c_str(), input_content.size()));
  }
  {
    base::ScopedFile file(
        base::OpenFile(upstream_path, O_CREAT | O_WRONLY, 0600));
    ASSERT_TRUE(file);
    ASSERT_TRUE(base::WriteAll(*file, upstream_content.c_str(),
                               upstream_content.size()));
  }

  protozero::MultiFileErrorCollectorImpl mfe;
  google::protobuf::compiler::DiskSourceTree dst;
  dst.MapPath("", temp_dir.path());
  dst.MapPath("", ".");
  dst.MapPath("", "buildtools/protobuf/src");

  google::protobuf::compiler::Importer importer_input(&dst, &mfe);
  const auto* input_desc = importer_input.Import("input.proto");

  google::protobuf::compiler::Importer importer_upstream(&dst, &mfe);
  const auto* upstream_desc = importer_upstream.Import("upstream.proto");

  ASSERT_NE(input_desc, nullptr);
  ASSERT_NE(upstream_desc, nullptr);

  Allowlist allowed;
  ASSERT_TRUE(
      AllowlistFromPassthrough(*input_desc, *upstream_desc, allowed).ok());

  ProtoFile input_file = ProtoFileFromDescriptor("", *input_desc);
  ProtoFile upstream_file = ProtoFileFromDescriptor("", *upstream_desc);

  ProtoFile merged;
  ASSERT_TRUE(MergeProtoFiles(input_file, upstream_file, allowed, merged).ok());

  std::string out = ProtoFileToDotProto(merged);
  EXPECT_THAT(out, HasSubstr("int32 f1 = 1;"));
  EXPECT_THAT(out, HasSubstr("string f2 = 2;"));
  EXPECT_THAT(out, HasSubstr("proto_filter_merge_passthrough) = true"));
}

TEST(ProtoFileSerializerTest, PassthroughDeepRecursion) {
  struct ScopedUnlink {
    std::string path;
    ~ScopedUnlink() { base::Unlink(path.c_str()); }
  };

  base::TempDir temp_dir = base::TempDir::Create();
  std::string input_path = temp_dir.path() + "/input.proto";
  std::string upstream_path = temp_dir.path() + "/upstream.proto";

  ScopedUnlink unlink_input{input_path};
  ScopedUnlink unlink_upstream{upstream_path};

  std::string input_content = R"(
    syntax = "proto2";
    package perfetto.protos;
    import "protos/perfetto/common/passthrough.proto";

    message Deep2 {
      optional int32 f1 = 1;
    }

    message Deep1 {
      optional Deep2 d2 = 1;
    }

    message Root {
      optional Deep1 d1 = 1 [(perfetto.protos.proto_filter_merge_passthrough) = true];
    }
  )";

  std::string upstream_content = R"(
    syntax = "proto2";
    package perfetto.protos;

    message Deep2 {
      optional int32 f1 = 1;
      optional string f2 = 2; // NEW
    }

    message Deep1 {
      optional Deep2 d2 = 1;
      optional int64 f3 = 2; // NEW
    }

    message Root {
      optional Deep1 d1 = 1;
    }
  )";

  {
    base::ScopedFile file(base::OpenFile(input_path, O_CREAT | O_WRONLY, 0600));
    ASSERT_TRUE(file);
    ASSERT_TRUE(
        base::WriteAll(*file, input_content.c_str(), input_content.size()));
  }
  {
    base::ScopedFile file(
        base::OpenFile(upstream_path, O_CREAT | O_WRONLY, 0600));
    ASSERT_TRUE(file);
    ASSERT_TRUE(base::WriteAll(*file, upstream_content.c_str(),
                               upstream_content.size()));
  }

  protozero::MultiFileErrorCollectorImpl mfe;
  google::protobuf::compiler::DiskSourceTree dst;
  dst.MapPath("", temp_dir.path());
  dst.MapPath("", ".");
  dst.MapPath("", "buildtools/protobuf/src");

  google::protobuf::compiler::Importer importer_input(&dst, &mfe);
  const auto* input_desc = importer_input.Import("input.proto");

  google::protobuf::compiler::Importer importer_upstream(&dst, &mfe);
  const auto* upstream_desc = importer_upstream.Import("upstream.proto");

  ASSERT_NE(input_desc, nullptr);
  ASSERT_NE(upstream_desc, nullptr);

  Allowlist allowed;
  ASSERT_TRUE(
      AllowlistFromPassthrough(*input_desc, *upstream_desc, allowed).ok());

  ProtoFile input_file = ProtoFileFromDescriptor("", *input_desc);
  ProtoFile upstream_file = ProtoFileFromDescriptor("", *upstream_desc);

  ProtoFile merged;
  ASSERT_TRUE(MergeProtoFiles(input_file, upstream_file, allowed, merged).ok());

  std::string out = ProtoFileToDotProto(merged);
  EXPECT_THAT(out, HasSubstr("int32 f1 = 1;"));
  EXPECT_THAT(out, HasSubstr("string f2 = 2;"));
  EXPECT_THAT(out, HasSubstr("int64 f3 = 2;"));
  EXPECT_THAT(out, HasSubstr("proto_filter_merge_passthrough) = true"));
}

}  // namespace
}  // namespace proto_merger
}  // namespace perfetto
