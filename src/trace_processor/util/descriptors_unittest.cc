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

#include "src/trace_processor/util/descriptors.h"

#include <cstdint>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
using FileDescriptorSet = protos::pbzero::FileDescriptorSet;

// Builds a descriptor set with two extension messages registered at the same
// extendee tag. Each extension message contains a *nested* message-typed
// field, so verifying their equivalence forces DescriptorsStructurallyEqual
// to perform a deep comparison using its worklist. `deep_difference` controls
// whether the difference (when present) lives one level deep inside the nested
// message rather than at the top level.
std::vector<uint8_t> BuildDescriptorSet(bool structurally_equal_extensions,
                                        bool deep_difference) {
  protozero::HeapBuffered<FileDescriptorSet> fds;
  auto* file = fds->add_file();
  file->set_name("test.proto");
  file->set_package("test");

  auto* base_msg = file->add_message_type();
  base_msg->set_name("BaseMessage");

  // Inner messages that the extension messages point at. InnerA and InnerB
  // are structurally identical unless `deep_difference` is set, in which case
  // InnerB's field differs one level below the extension message.
  auto* inner_a = file->add_message_type();
  inner_a->set_name("InnerA");
  auto* inner_a_field = inner_a->add_field();
  inner_a_field->set_name("leaf");
  inner_a_field->set_number(1);
  inner_a_field->set_type(FieldDescriptorProto::TYPE_INT32);
  inner_a_field->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

  auto* inner_b = file->add_message_type();
  inner_b->set_name("InnerB");
  auto* inner_b_field = inner_b->add_field();
  inner_b_field->set_name("leaf");
  inner_b_field->set_number(1);
  if (structurally_equal_extensions || !deep_difference) {
    inner_b_field->set_type(FieldDescriptorProto::TYPE_INT32);
  } else {
    // Difference one level deep, inside the nested message.
    inner_b_field->set_type(FieldDescriptorProto::TYPE_STRING);
  }
  inner_b_field->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

  // ExtMsgA: has a message-typed field pointing at InnerA.
  auto* ext_msg_a = file->add_message_type();
  ext_msg_a->set_name("ExtMsgA");
  auto* field_a = ext_msg_a->add_field();
  field_a->set_name("nested");
  field_a->set_number(1);
  field_a->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  field_a->set_type_name(".test.InnerA");
  field_a->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

  // ExtMsgB: same shape, points at InnerB. When a non-deep difference is
  // requested, the top-level field type itself differs instead.
  auto* ext_msg_b = file->add_message_type();
  ext_msg_b->set_name("ExtMsgB");
  auto* field_b = ext_msg_b->add_field();
  field_b->set_name("nested");
  field_b->set_number(1);
  if (structurally_equal_extensions || deep_difference) {
    field_b->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    field_b->set_type_name(".test.InnerB");
  } else {
    // Shallow (top-level) difference.
    field_b->set_type(FieldDescriptorProto::TYPE_STRING);
  }
  field_b->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

  auto* ext1 = file->add_extension();
  ext1->set_name("ext_field");
  ext1->set_number(10);
  ext1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext1->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  ext1->set_type_name(".test.ExtMsgA");
  ext1->set_extendee(".test.BaseMessage");

  auto* ext2 = file->add_extension();
  ext2->set_name("ext_field");
  ext2->set_number(10);
  ext2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext2->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  ext2->set_type_name(".test.ExtMsgB");
  ext2->set_extendee(".test.BaseMessage");

  return fds.SerializeAsArray();
}

// Builds a descriptor set with two extension messages that are each
// self-referential (a field of the message points back at the message
// itself), registered at the same extendee tag. This forces the cycle guard
// in DescriptorsStructurallyEqual to trigger; without it the comparison
// would not terminate.
std::vector<uint8_t> BuildSelfReferentialDescriptorSet() {
  protozero::HeapBuffered<FileDescriptorSet> fds;
  auto* file = fds->add_file();
  file->set_name("test.proto");
  file->set_package("test");

  auto* base_msg = file->add_message_type();
  base_msg->set_name("BaseMessage");

  auto* rec_a = file->add_message_type();
  rec_a->set_name("RecA");
  auto* rec_a_self = rec_a->add_field();
  rec_a_self->set_name("self");
  rec_a_self->set_number(1);
  rec_a_self->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  rec_a_self->set_type_name(".test.RecA");
  rec_a_self->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

  auto* rec_b = file->add_message_type();
  rec_b->set_name("RecB");
  auto* rec_b_self = rec_b->add_field();
  rec_b_self->set_name("self");
  rec_b_self->set_number(1);
  rec_b_self->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  rec_b_self->set_type_name(".test.RecB");
  rec_b_self->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

  auto* ext1 = file->add_extension();
  ext1->set_name("ext_field");
  ext1->set_number(10);
  ext1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext1->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  ext1->set_type_name(".test.RecA");
  ext1->set_extendee(".test.BaseMessage");

  auto* ext2 = file->add_extension();
  ext2->set_name("ext_field");
  ext2->set_number(10);
  ext2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext2->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  ext2->set_type_name(".test.RecB");
  ext2->set_extendee(".test.BaseMessage");

  return fds.SerializeAsArray();
}

// Builds a descriptor set that re-declares an extension at the same tag in a
// way controlled by the arguments, to exercise the non-deferred branches of
// CheckExtensionField:
//  - `second_is_scalar_mismatch`: the second declaration uses a different
//    fundamental type (int32 vs the first's message), which must be rejected
//    immediately.
//  - otherwise the second declaration is byte-identical to the first
//    (same tag, same type, same type name): a compatible re-declaration that
//    must be accepted without deferring a structural check.
std::vector<uint8_t> BuildReDeclDescriptorSet(bool second_is_scalar_mismatch) {
  protozero::HeapBuffered<FileDescriptorSet> fds;
  auto* file = fds->add_file();
  file->set_name("test.proto");
  file->set_package("test");

  auto* base_msg = file->add_message_type();
  base_msg->set_name("BaseMessage");

  auto* ext_msg = file->add_message_type();
  ext_msg->set_name("ExtMsg");
  auto* ext_msg_field = ext_msg->add_field();
  ext_msg_field->set_name("val");
  ext_msg_field->set_number(1);
  ext_msg_field->set_type(FieldDescriptorProto::TYPE_INT32);
  ext_msg_field->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

  auto* ext1 = file->add_extension();
  ext1->set_name("ext_field");
  ext1->set_number(10);
  ext1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext1->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  ext1->set_type_name(".test.ExtMsg");
  ext1->set_extendee(".test.BaseMessage");

  auto* ext2 = file->add_extension();
  ext2->set_name("ext_field");
  ext2->set_number(10);
  ext2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext2->set_extendee(".test.BaseMessage");
  if (second_is_scalar_mismatch) {
    // Different fundamental type at the same tag: rejected immediately by
    // CheckExtensionField, before any deferred structural check.
    ext2->set_type(FieldDescriptorProto::TYPE_INT32);
  } else {
    // Identical re-declaration: same tag, same type, same type name. Must
    // be accepted with no deferral.
    ext2->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    ext2->set_type_name(".test.ExtMsg");
  }

  return fds.SerializeAsArray();
}

// Builds a descriptor set with two enum-typed extensions registered at the
// same tag, pointing at two differently-named enums. When `enums_equal` is
// true the two enums have identical values (a pure rename); otherwise one
// value's number differs.
std::vector<uint8_t> BuildEnumExtDescriptorSet(bool enums_equal) {
  protozero::HeapBuffered<FileDescriptorSet> fds;
  auto* file = fds->add_file();
  file->set_name("test.proto");
  file->set_package("test");

  auto* base_msg = file->add_message_type();
  base_msg->set_name("BaseMessage");

  auto* enum_a = file->add_enum_type();
  enum_a->set_name("EnumA");
  auto* a0 = enum_a->add_value();
  a0->set_name("UNKNOWN");
  a0->set_number(0);
  auto* a1 = enum_a->add_value();
  a1->set_name("RUNNING");
  a1->set_number(1);

  auto* enum_b = file->add_enum_type();
  enum_b->set_name("EnumB");
  auto* b0 = enum_b->add_value();
  b0->set_name("UNKNOWN");
  b0->set_number(0);
  auto* b1 = enum_b->add_value();
  b1->set_name(enums_equal ? "RUNNING" : "STOPPED");
  b1->set_number(1);

  auto* ext1 = file->add_extension();
  ext1->set_name("ext_field");
  ext1->set_number(10);
  ext1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext1->set_type(FieldDescriptorProto::TYPE_ENUM);
  ext1->set_type_name(".test.EnumA");
  ext1->set_extendee(".test.BaseMessage");

  auto* ext2 = file->add_extension();
  ext2->set_name("ext_field");
  ext2->set_number(10);
  ext2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext2->set_type(FieldDescriptorProto::TYPE_ENUM);
  ext2->set_type_name(".test.EnumB");
  ext2->set_extendee(".test.BaseMessage");

  return fds.SerializeAsArray();
}

// Builds a descriptor set with two enum-typed extensions at the same tag.
// Both enums share value 0 -> "UNKNOWN". `extra_on_first` adds value
// 2 -> "EXTRA" to the first (existing) enum only; otherwise it is added to
// the second (candidate) enum only. If `conflict_shared` is true, the shared
// value 0 is given a different name on the second enum, which must be
// rejected regardless of the extra-value placement.
std::vector<uint8_t> BuildEnumSupersetExtDescriptorSet(bool extra_on_first,
                                                       bool conflict_shared) {
  protozero::HeapBuffered<FileDescriptorSet> fds;
  auto* file = fds->add_file();
  file->set_name("test.proto");
  file->set_package("test");

  auto* base_msg = file->add_message_type();
  base_msg->set_name("BaseMessage");

  auto* enum_a = file->add_enum_type();
  enum_a->set_name("EnumA");
  auto* a0 = enum_a->add_value();
  a0->set_name("UNKNOWN");
  a0->set_number(0);
  if (extra_on_first) {
    auto* a2 = enum_a->add_value();
    a2->set_name("EXTRA");
    a2->set_number(2);
  }

  auto* enum_b = file->add_enum_type();
  enum_b->set_name("EnumB");
  auto* b0 = enum_b->add_value();
  b0->set_name(conflict_shared ? "DIFFERENT" : "UNKNOWN");
  b0->set_number(0);
  if (!extra_on_first) {
    auto* b2 = enum_b->add_value();
    b2->set_name("EXTRA");
    b2->set_number(2);
  }

  auto* ext1 = file->add_extension();
  ext1->set_name("ext_field");
  ext1->set_number(10);
  ext1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext1->set_type(FieldDescriptorProto::TYPE_ENUM);
  ext1->set_type_name(".test.EnumA");
  ext1->set_extendee(".test.BaseMessage");

  auto* ext2 = file->add_extension();
  ext2->set_name("ext_field");
  ext2->set_number(10);
  ext2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext2->set_type(FieldDescriptorProto::TYPE_ENUM);
  ext2->set_type_name(".test.EnumB");
  ext2->set_extendee(".test.BaseMessage");

  return fds.SerializeAsArray();
}

// Builds a descriptor set with two extension messages at the same tag whose
// shared field (tag 1) is identical, but one side has an extra field (tag 2)
// that the other lacks. `extra_on_existing` puts the extra field on the
// first-declared (existing) side; otherwise it is on the re-declared
// (candidate) side. Used to verify that a superset on either side is
// accepted, while the shared field still matches.
std::vector<uint8_t> BuildSupersetExtDescriptorSet(bool extra_on_existing) {
  protozero::HeapBuffered<FileDescriptorSet> fds;
  auto* file = fds->add_file();
  file->set_name("test.proto");
  file->set_package("test");

  auto* base_msg = file->add_message_type();
  base_msg->set_name("BaseMessage");

  auto* ext_msg_a = file->add_message_type();
  ext_msg_a->set_name("ExtMsgA");
  auto* a1 = ext_msg_a->add_field();
  a1->set_name("shared");
  a1->set_number(1);
  a1->set_type(FieldDescriptorProto::TYPE_INT32);
  a1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  if (extra_on_existing) {
    auto* a2 = ext_msg_a->add_field();
    a2->set_name("extra");
    a2->set_number(2);
    a2->set_type(FieldDescriptorProto::TYPE_INT32);
    a2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  }

  auto* ext_msg_b = file->add_message_type();
  ext_msg_b->set_name("ExtMsgB");
  auto* b1 = ext_msg_b->add_field();
  b1->set_name("shared");
  b1->set_number(1);
  b1->set_type(FieldDescriptorProto::TYPE_INT32);
  b1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  if (!extra_on_existing) {
    auto* b2 = ext_msg_b->add_field();
    b2->set_name("extra");
    b2->set_number(2);
    b2->set_type(FieldDescriptorProto::TYPE_INT32);
    b2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  }

  auto* ext1 = file->add_extension();
  ext1->set_name("ext_field");
  ext1->set_number(10);
  ext1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext1->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  ext1->set_type_name(".test.ExtMsgA");
  ext1->set_extendee(".test.BaseMessage");

  auto* ext2 = file->add_extension();
  ext2->set_name("ext_field");
  ext2->set_number(10);
  ext2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext2->set_type(FieldDescriptorProto::TYPE_MESSAGE);
  ext2->set_type_name(".test.ExtMsgB");
  ext2->set_extendee(".test.BaseMessage");

  return fds.SerializeAsArray();
}

// Two differently-named extension messages that are recursively identical
// (including through a nested message) are accepted, and the field resolves.
TEST(DescriptorsTest, IdenticalExtensionReDeclarationAllowed) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes =
      BuildDescriptorSet(/*structurally_equal_extensions=*/true,
                         /*deep_difference=*/false);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_TRUE(status.ok()) << status.message();

  auto base_idx = pool.FindDescriptorIdx(".test.BaseMessage");
  ASSERT_TRUE(base_idx.has_value());
  const auto& desc = pool.descriptors()[base_idx.value()];
  const auto* field = desc.FindFieldByTag(10);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(field->name(), "ext_field");
}

// A difference at the top level of the extension message is rejected.
TEST(DescriptorsTest, NonIdenticalExtensionReDeclarationRejected) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes =
      BuildDescriptorSet(/*structurally_equal_extensions=*/false,
                         /*deep_difference=*/false);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.c_message(),
              testing::HasSubstr("not structurally identical"));
}

// A difference one level deep inside a nested message is rejected. This
// proves the comparison genuinely descends into nested types, not just the
// top level.
TEST(DescriptorsTest, DeepNestedDifferenceRejected) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes =
      BuildDescriptorSet(/*structurally_equal_extensions=*/false,
                         /*deep_difference=*/true);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.c_message(),
              testing::HasSubstr("not structurally identical"));
}

// Self-referential (cyclic) messages that are structurally identical must
// terminate and be accepted, exercising the coinductive cycle guard.
TEST(DescriptorsTest, SelfReferentialIdenticalExtensionAllowed) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes = BuildSelfReferentialDescriptorSet();
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_TRUE(status.ok()) << status.message();
}

// CheckExtensionField: re-declaring an extension at the same tag with a
// different fundamental type (message vs int32) is rejected immediately,
// independent of the structural-equality path.
TEST(DescriptorsTest, ExtensionReDeclaredWithDifferentFundamentalTypeRejected) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes =
      BuildReDeclDescriptorSet(/*second_is_scalar_mismatch=*/true);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.c_message(),
              testing::HasSubstr("re-introduced with different type"));
}

// CheckExtensionField: re-declaring an extension at the same tag with an
// identical type (same name, same kind) is accepted and does not trigger a
// deferred structural check.
TEST(DescriptorsTest, IdenticalExtensionReDeclarationSameNameAllowed) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes =
      BuildReDeclDescriptorSet(/*second_is_scalar_mismatch=*/false);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_TRUE(status.ok()) << status.message();
  auto base_idx = pool.FindDescriptorIdx(".test.BaseMessage");
  ASSERT_TRUE(base_idx.has_value());
  const auto* field = pool.descriptors()[base_idx.value()].FindFieldByTag(10);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(field->name(), "ext_field");
}

// CheckExtensionField: a descriptor set with no extension re-declaration at
// all exercises the "no existing field at this tag" early return for a
// genuine extension (not just incidentally via non-extension fields).
TEST(DescriptorsTest, SingleExtensionDeclarationAllowed) {
  protozero::HeapBuffered<FileDescriptorSet> fds;
  auto* file = fds->add_file();
  file->set_name("test.proto");
  file->set_package("test");
  auto* base_msg = file->add_message_type();
  base_msg->set_name("BaseMessage");
  auto* ext = file->add_extension();
  ext->set_name("ext_field");
  ext->set_number(10);
  ext->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
  ext->set_type(FieldDescriptorProto::TYPE_INT32);
  ext->set_extendee(".test.BaseMessage");
  std::vector<uint8_t> fds_bytes = fds.SerializeAsArray();

  DescriptorPool pool;
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_TRUE(status.ok()) << status.message();
  auto base_idx = pool.FindDescriptorIdx(".test.BaseMessage");
  ASSERT_TRUE(base_idx.has_value());
  EXPECT_NE(pool.descriptors()[base_idx.value()].FindFieldByTag(10), nullptr);
}

// A renamed enum with identical values is accepted.
TEST(DescriptorsTest, IdenticalEnumExtensionReDeclarationAllowed) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes =
      BuildEnumExtDescriptorSet(/*enums_equal=*/true);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_TRUE(status.ok()) << status.message();
  auto base_idx = pool.FindDescriptorIdx(".test.BaseMessage");
  ASSERT_TRUE(base_idx.has_value());
  EXPECT_NE(pool.descriptors()[base_idx.value()].FindFieldByTag(10), nullptr);
}

// A renamed enum whose shared value number maps to a different name is a
// genuine conflict and is rejected. (Differing value *sets* without a shared
// conflict are allowed; that case is covered by the superset tests.)
TEST(DescriptorsTest, ConflictingEnumValueReDeclarationRejected) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes =
      BuildEnumExtDescriptorSet(/*enums_equal=*/false);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.c_message(),
              testing::HasSubstr("not structurally identical"));
}

// The re-declared (candidate) side has an extra field the existing side
// lacks. This is the Android-adds-a-field case and must be accepted.
TEST(DescriptorsTest, SupersetOnCandidateSideAllowed) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes =
      BuildSupersetExtDescriptorSet(/*extra_on_existing=*/false);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_TRUE(status.ok()) << status.message();
  auto base_idx = pool.FindDescriptorIdx(".test.BaseMessage");
  ASSERT_TRUE(base_idx.has_value());
  EXPECT_NE(pool.descriptors()[base_idx.value()].FindFieldByTag(10), nullptr);
}

// The existing side has an extra field the re-declared side lacks. With the
// deprecate-in-place convention this is also a superset relationship and
// must be accepted (symmetric).
TEST(DescriptorsTest, SupersetOnExistingSideAllowed) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes =
      BuildSupersetExtDescriptorSet(/*extra_on_existing=*/true);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_TRUE(status.ok()) << status.message();
}

// Candidate enum has an extra value the existing enum lacks: accepted.
TEST(DescriptorsTest, EnumSupersetOnCandidateSideAllowed) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes = BuildEnumSupersetExtDescriptorSet(
      /*extra_on_first=*/false, /*conflict_shared=*/false);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_TRUE(status.ok()) << status.message();
}

// Existing enum has an extra value the candidate enum lacks: also accepted
// (symmetric).
TEST(DescriptorsTest, EnumSupersetOnExistingSideAllowed) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes = BuildEnumSupersetExtDescriptorSet(
      /*extra_on_first=*/true, /*conflict_shared=*/false);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_TRUE(status.ok()) << status.message();
}

// A shared enum number with a different name is a genuine conflict and is
// still rejected even though one side also has an extra value.
TEST(DescriptorsTest, EnumConflictingSharedValueRejected) {
  DescriptorPool pool;
  std::vector<uint8_t> fds_bytes = BuildEnumSupersetExtDescriptorSet(
      /*extra_on_first=*/false, /*conflict_shared=*/true);
  auto status =
      pool.AddFromFileDescriptorSet(fds_bytes.data(), fds_bytes.size());
  EXPECT_FALSE(status.ok());
  EXPECT_THAT(status.c_message(),
              testing::HasSubstr("not structurally identical"));
}

}  // namespace
}  // namespace perfetto::trace_processor
