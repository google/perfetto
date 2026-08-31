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
#include "src/tools/proto_merger/extension_proto_merger.h"
#include "src/tools/proto_merger/proto_merger.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace proto_merger {
namespace {

using testing::HasSubstr;
using testing::Not;

class TempProtoFile {
 public:
  TempProtoFile(const std::string& temp_dir,
                const std::string& name,
                const std::string& content)
      : path_(temp_dir + "/" + name) {
    base::ScopedFile file(base::OpenFile(path_, O_CREAT | O_WRONLY, 0600));
    PERFETTO_CHECK(file);
    PERFETTO_CHECK(base::WriteAll(*file, content.c_str(), content.size()));
  }

  ~TempProtoFile() { base::Unlink(path_.c_str()); }

 private:
  std::string path_;
};

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

TEST(ProtoFileSerializerTest, ReservedUpstreamFieldIsMarkedDeprecated) {
  ProtoFile input;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("int32", "active_field_ten", 10));
    message.fields.push_back(MakeField("string", "reserved_field", 2));
    message.fields.push_back(MakeField("int32", "active_field_five", 5));
    message.fields.push_back(MakeField("bool", "truly_deleted_field", 3));
    input.messages.push_back(message);
  }

  ProtoFile upstream;
  {
    ProtoFile::Message message{};
    message.name = "Container";
    message.fields.push_back(MakeField("int32", "active_field_ten", 10));
    message.fields.push_back(MakeField("int32", "active_field_five", 5));

    // Field 2 is marked reserved upstream (not in fields list)
    message.reserved_numbers.insert(2);
    // Field 3 was removed completely without reserving

    upstream.messages.push_back(message);
  }

  ProtoFile merged;
  ASSERT_TRUE(MergeProtoFiles(input, upstream, Allowlist{}, merged).ok());

  std::string out = ProtoFileToDotProto(merged);

  // 1. Active fields preserve upstream declaration order (10 before 5)
  size_t pos_10 = out.find("int32 active_field_ten = 10;");
  size_t pos_5 = out.find("int32 active_field_five = 5;");
  EXPECT_NE(pos_10, std::string::npos);
  EXPECT_NE(pos_5, std::string::npos);
  EXPECT_LT(pos_10, pos_5);

  // 2. reserved_field is kept in active fields with [deprecated = true]
  EXPECT_THAT(out, HasSubstr("string reserved_field = 2 [deprecated = true];"));

  // 3. truly_deleted_field is placed in the commented-out section
  EXPECT_THAT(out, HasSubstr("bool truly_deleted_field = 3;"));
  EXPECT_THAT(out, HasSubstr("not present upstream"));
}

TEST(ProtoFileSerializerTest, EndToEndReservedFieldMerge) {
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

    // --- PREAMBLE ENDS HERE - EVERYTHING BELOW AUTOGENERATED ---

    message Config {
      optional int32 active_field = 1;
      optional string legacy_field = 2;
      optional bool deleted_field = 3;
    }
  )";

  std::string upstream_content = R"(
    syntax = "proto2";
    package perfetto.protos;

    message Config {
      optional int32 active_field = 1;
      reserved 2;
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
  // Use google::protobuf::compiler here:
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

  ProtoFile input_file = ProtoFileFromDescriptor("", *input_desc);
  ProtoFile upstream_file = ProtoFileFromDescriptor("", *upstream_desc);

  ProtoFile merged;
  ASSERT_TRUE(
      MergeProtoFiles(input_file, upstream_file, Allowlist{}, merged).ok());

  std::string out = ProtoFileToDotProto(merged);

  EXPECT_THAT(out, HasSubstr("int32 active_field = 1;"));
  EXPECT_THAT(out, HasSubstr("string legacy_field = 2 [deprecated = true];"));
  EXPECT_THAT(out, HasSubstr("bool deleted_field = 3;"));
  EXPECT_THAT(out, HasSubstr("not present upstream"));
}

TEST(ProtoFileSerializerTest, MapFieldsDoNotGetRepeatedOrEntryMessages) {
  struct ScopedUnlink {
    std::string path;
    ~ScopedUnlink() { base::Unlink(path.c_str()); }
  };

  base::TempDir temp_dir = base::TempDir::Create();
  std::string input_path = temp_dir.path() + "/input.proto";
  std::string upstream_path = temp_dir.path() + "/upstream.proto";
  std::string merged_path = temp_dir.path() + "/merged.proto";

  ScopedUnlink unlink_input{input_path};
  ScopedUnlink unlink_upstream{upstream_path};
  ScopedUnlink unlink_merged{merged_path};

  std::string input_content = R"(
    syntax = "proto3";
    package perfetto.protos;

    message SubMsg {
      string name = 1;
    }

    message Container {
      map<string, int32> simple_map = 1;
      map<string, SubMsg> msg_map = 2;
    }
  )";

  std::string upstream_content = R"(
    syntax = "proto3";
    package perfetto.protos;

    message SubMsg {
      string name = 1;
    }

    message Container {
      map<string, int32> simple_map = 1;
      map<string, SubMsg> msg_map = 2;
      map<int64, string> new_map = 3;
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

  protozero::MultiFileErrorCollectorImpl mfe_input;
  protozero::MultiFileErrorCollectorImpl mfe_upstream;
  protozero::MultiFileErrorCollectorImpl mfe_merged;
  google::protobuf::compiler::DiskSourceTree dst;
  dst.MapPath("", temp_dir.path());
  dst.MapPath("", ".");
  dst.MapPath("", "buildtools/protobuf/src");

  google::protobuf::compiler::Importer importer_input(&dst, &mfe_input);
  const auto* input_desc = importer_input.Import("input.proto");

  google::protobuf::compiler::Importer importer_upstream(&dst, &mfe_upstream);
  const auto* upstream_desc = importer_upstream.Import("upstream.proto");

  ASSERT_NE(input_desc, nullptr);
  ASSERT_NE(upstream_desc, nullptr);

  Allowlist allowed;
  const auto* container_desc =
      upstream_desc->FindMessageTypeByName("Container");
  ASSERT_NE(container_desc, nullptr);
  ASSERT_TRUE(AllowlistFromFieldList(*container_desc,
                                     {"simple_map", "msg_map", "new_map"},
                                     allowed)
                  .ok());

  ProtoFile input_file = ProtoFileFromDescriptor("", *input_desc);
  ProtoFile upstream_file = ProtoFileFromDescriptor("", *upstream_desc);

  ProtoFile merged;
  ASSERT_TRUE(MergeProtoFiles(input_file, upstream_file, allowed, merged).ok());

  std::string out = ProtoFileToDotProto(merged);

  EXPECT_THAT(out, HasSubstr("map<string,int32> simple_map = 1;"));
  EXPECT_THAT(out, HasSubstr("map<string,SubMsg> msg_map = 2;"));
  EXPECT_THAT(out, HasSubstr("map<int64,string> new_map = 3;"));
  EXPECT_THAT(out, Not(HasSubstr("repeated map")));
  EXPECT_THAT(out, Not(HasSubstr("SimpleMapEntry")));
  EXPECT_THAT(out, Not(HasSubstr("MsgMapEntry")));
  EXPECT_THAT(out, Not(HasSubstr("NewMapEntry")));

  // Verify the serialized proto can be parsed by protoc importer without error.
  std::string full_proto =
      "syntax = \"proto3\";\npackage perfetto.protos;\n" + out;
  {
    base::ScopedFile file(
        base::OpenFile(merged_path, O_CREAT | O_WRONLY, 0600));
    ASSERT_TRUE(file);
    ASSERT_TRUE(base::WriteAll(*file, full_proto.c_str(), full_proto.size()));
  }

  google::protobuf::compiler::Importer importer_merged(&dst, &mfe_merged);
  const auto* merged_desc = importer_merged.Import("merged.proto");
  EXPECT_NE(merged_desc, nullptr);
}

TEST(ProtoFileSerializerTest, ExtensionProtoMergerInlinesAllExtensions) {
  base::TempDir temp_dir = base::TempDir::Create();
  std::string base_content = R"(
    syntax = "proto2";
    package perfetto.protos;

    message TrackEvent {
      optional string name = 1;
      extensions 1000 to 9999;
    }

    message TracePacket {
      optional uint64 timestamp = 1;
      extensions 1000 to 9999;
    }

    message InternedData {
      extensions 1000 to 9999;
    }
  )";

  std::string ext1_content = R"(
    syntax = "proto2";
    package com.android.internal;
    import "base.proto";

    enum ReceiverType {
      RECEIVER_TYPE_UNKNOWN = 0;
      RECEIVER_TYPE_RUNTIME = 1;
    }

    message AndroidMessageQueue {
      optional string sending_thread_name = 1;
    }

    extend perfetto.protos.TrackEvent {
      optional AndroidMessageQueue message_queue = 2004;
      optional ReceiverType receiver_type = 2005;
    }
  )";

  std::string ext2_content = R"(
    syntax = "proto2";
    package com.android.trace;
    import "base.proto";

    message AppWakelockBundle {
      optional string tag = 1;
    }

    extend perfetto.protos.TracePacket {
      optional AppWakelockBundle app_wakelock = 1100;
    }

    extend perfetto.protos.InternedData {
      repeated string interned_str = 1200;
    }
  )";

  TempProtoFile temp_base(temp_dir.path(), "base.proto", base_content);
  TempProtoFile temp_ext1(temp_dir.path(), "ext1.proto", ext1_content);
  TempProtoFile temp_ext2(temp_dir.path(), "ext2.proto", ext2_content);

  protozero::MultiFileErrorCollectorImpl mfe;
  google::protobuf::compiler::DiskSourceTree dst;
  dst.MapPath("", temp_dir.path());
  dst.MapPath("", ".");
  dst.MapPath("", "buildtools/protobuf/src");

  google::protobuf::compiler::Importer importer(&dst, &mfe);
  const auto* base_desc = importer.Import("base.proto");
  const auto* ext1_desc = importer.Import("ext1.proto");
  const auto* ext2_desc = importer.Import("ext2.proto");

  ASSERT_NE(base_desc, nullptr);
  ASSERT_NE(ext1_desc, nullptr);
  ASSERT_NE(ext2_desc, nullptr);

  ProtoFile base_file = ProtoFileFromDescriptor(
      "syntax = \"proto2\";\npackage perfetto.protos;\n\n", *base_desc,
      {ext1_desc, ext2_desc});

  std::string out = ProtoFileToDotProto(base_file);

  // Check TrackEvent extensions
  EXPECT_THAT(out, HasSubstr("message TrackEvent {"));
  EXPECT_THAT(out,
              HasSubstr("optional AndroidMessageQueue message_queue = 2004;"));
  EXPECT_THAT(out, HasSubstr("optional ReceiverType receiver_type = 2005;"));

  // Check TracePacket extensions
  EXPECT_THAT(out, HasSubstr("message TracePacket {"));
  EXPECT_THAT(out,
              HasSubstr("optional AppWakelockBundle app_wakelock = 1100;"));

  // Check InternedData extensions
  EXPECT_THAT(out, HasSubstr("message InternedData {"));
  EXPECT_THAT(out, HasSubstr("repeated string interned_str = 1200;"));

  // Check helper messages and enums relocated
  EXPECT_THAT(out, HasSubstr("enum ReceiverType {"));
  EXPECT_THAT(out, HasSubstr("message AndroidMessageQueue {"));
  EXPECT_THAT(out, HasSubstr("message AppWakelockBundle {"));
}

TEST(ExtensionProtoMergerTest, ExtractPreambleWithMarker) {
  std::string content =
      "syntax = \"proto2\";\n"
      "package perfetto.protos;\n"
      "// --- PREAMBLE ENDS HERE - EVERYTHING BELOW AUTOGENERATED ---\n"
      "message Foo {\n"
      "  optional int32 x = 1;\n"
      "}\n";
  std::string preamble = ExtractPreamble(content);
  EXPECT_THAT(preamble, HasSubstr("package perfetto.protos;"));
  EXPECT_THAT(preamble, HasSubstr("// --- PREAMBLE ENDS HERE"));
  EXPECT_THAT(preamble, Not(HasSubstr("message Foo")));
}

TEST(ExtensionProtoMergerTest, ExtractPreambleWithDirectDefinitions) {
  std::string content =
      "// Copyright header\n"
      "syntax = \"proto2\";\n"
      "package perfetto.protos;\n\n"
      "// Begin of some block\n"
      "message Foo {\n"
      "  optional int32 x = 1;\n"
      "}\n";
  std::string preamble = ExtractPreamble(content);
  EXPECT_THAT(preamble, HasSubstr("// Copyright header"));
  EXPECT_THAT(preamble, HasSubstr("package perfetto.protos;"));
  EXPECT_THAT(preamble, Not(HasSubstr("message Foo")));
}

TEST(ExtensionProtoMergerTest, MergeExtensionsEndToEnd) {
  base::TempDir temp_dir = base::TempDir::Create();
  std::string base_content = R"(
    syntax = "proto2";
    package perfetto.protos;

    message TrackEvent {
      optional string name = 1;
      extensions 1000 to 9999;
    }

    message TracePacket {
      optional uint64 timestamp = 1;
      extensions 1000 to 9999;
    }
  )";

  std::string ext1_content = R"(
    syntax = "proto2";
    package com.android.internal;
    import "base.proto";

    enum ReceiverType {
      RECEIVER_TYPE_UNKNOWN = 0;
      RECEIVER_TYPE_RUNTIME = 1;
    }

    message AndroidMessageQueue {
      optional string sending_thread_name = 1;
    }

    extend perfetto.protos.TrackEvent {
      optional AndroidMessageQueue message_queue = 2004;
      optional ReceiverType receiver_type = 2005;
    }
  )";

  std::string ext2_content = R"(
    syntax = "proto2";
    package com.android.trace;
    import "base.proto";

    message AppWakelockBundle {
      optional string tag = 1;
    }

    extend perfetto.protos.TracePacket {
      optional AppWakelockBundle app_wakelock = 1100;
    }
  )";

  TempProtoFile temp_base(temp_dir.path(), "base.proto", base_content);
  TempProtoFile temp_ext1(temp_dir.path(), "ext1.proto", ext1_content);
  TempProtoFile temp_ext2(temp_dir.path(), "ext2.proto", ext2_content);

  std::string out;
  base::Status status = MergeExtensions("base.proto", temp_dir.path(),
                                        {"ext1.proto", "ext2.proto"}, &out);
  ASSERT_TRUE(status.ok()) << status.message();

  // Verify inlined fields in TrackEvent
  EXPECT_THAT(out, HasSubstr("message TrackEvent {"));
  EXPECT_THAT(out,
              HasSubstr("optional AndroidMessageQueue message_queue = 2004;"));
  EXPECT_THAT(out, HasSubstr("optional ReceiverType receiver_type = 2005;"));

  // Verify inlined fields in TracePacket
  EXPECT_THAT(out, HasSubstr("message TracePacket {"));
  EXPECT_THAT(out,
              HasSubstr("optional AppWakelockBundle app_wakelock = 1100;"));

  // Verify relocated helper types appear BEFORE their target message
  EXPECT_THAT(out, HasSubstr("enum ReceiverType {"));
  EXPECT_THAT(out, HasSubstr("message AndroidMessageQueue {"));
  EXPECT_THAT(out, HasSubstr("message AppWakelockBundle {"));
  EXPECT_LT(out.find("message AndroidMessageQueue {"),
            out.find("message TrackEvent {"));
  EXPECT_LT(out.find("message AppWakelockBundle {"),
            out.find("message TracePacket {"));
}

TEST(ExtensionProtoMergerTest, CrossFileExtensionDependency) {
  base::TempDir temp_dir = base::TempDir::Create();
  std::string base_content = R"(
    syntax = "proto2";
    package perfetto.protos;

    message TrackEvent {
      optional string name = 1;
      extensions 1000 to 9999;
    }
  )";

  std::string dep_content = R"(
    syntax = "proto2";
    package com.android.common;

    message SharedMeta {
      optional string key = 1;
      optional string value = 2;
    }
  )";

  std::string ext_content = R"(
    syntax = "proto2";
    package com.android.tracing;
    import "base.proto";
    import "dep.proto";

    extend perfetto.protos.TrackEvent {
      optional com.android.common.SharedMeta shared_meta = 3000;
    }
  )";

  TempProtoFile temp_base(temp_dir.path(), "base.proto", base_content);
  TempProtoFile temp_dep(temp_dir.path(), "dep.proto", dep_content);
  TempProtoFile temp_ext(temp_dir.path(), "ext.proto", ext_content);

  std::string out;
  base::Status status =
      MergeExtensions("base.proto", temp_dir.path(), {"ext.proto"}, &out);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_THAT(out, HasSubstr("message TrackEvent {"));
  EXPECT_THAT(out, HasSubstr("optional SharedMeta shared_meta = 3000;"));
  EXPECT_THAT(out, HasSubstr("message SharedMeta {"));
  EXPECT_LT(out.find("message SharedMeta {"), out.find("message TrackEvent {"));
}

TEST(ExtensionProtoMergerTest, UnassociatedHelpersAreOmitted) {
  base::TempDir temp_dir = base::TempDir::Create();
  std::string base_content = R"(
    syntax = "proto2";
    package perfetto.protos;

    message TrackEvent {
      optional string name = 1;
      extensions 1000 to 9999;
    }
  )";

  std::string ext_content = R"(
    syntax = "proto2";
    package com.android.unassociated;
    import "base.proto";

    message UnusedHelper {
      optional string data = 1;
    }

    message OtherMessage {
      extensions 100 to 200;
    }

    extend OtherMessage {
      optional UnusedHelper unused = 101;
    }
  )";

  TempProtoFile temp_base(temp_dir.path(), "base.proto", base_content);
  TempProtoFile temp_ext(temp_dir.path(), "ext.proto", ext_content);

  std::string out;
  base::Status status =
      MergeExtensions("base.proto", temp_dir.path(), {"ext.proto"}, &out);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_THAT(out, HasSubstr("message TrackEvent {"));
  EXPECT_THAT(out, Not(HasSubstr("UnusedHelper")));
  EXPECT_THAT(out, Not(HasSubstr("OtherMessage")));
}
}  // namespace
}  // namespace proto_merger
}  // namespace perfetto
