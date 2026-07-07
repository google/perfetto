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

#include "src/protozero/text_to_proto/text_to_proto.h"

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/protozero/test/example_proto/extensions.pbzero.h"
#include "src/protozero/test/example_proto/test_messages.pbzero.h"
#include "src/trace_processor/test_messages.descriptor.h"
#include "test/gtest_and_gmock.h"

namespace protozero {
namespace {

using ::perfetto::kTestMessagesDescriptor;
namespace pbtest = ::protozero::test::protos::pbzero;

perfetto::base::StatusOr<std::vector<uint8_t>> Parse(
    const std::string& root_type,
    const std::string& input) {
  return TextToProto(kTestMessagesDescriptor.data(),
                     kTestMessagesDescriptor.size(), root_type, "test", input);
}

TEST(TextToProtoTest, RegularField) {
  auto result = Parse(".protozero.test.protos.RealFakeEvent",
                      R"(base_int: 7
                         base_string: "hello")");
  ASSERT_TRUE(result.ok()) << result.status().message();
  pbtest::RealFakeEvent::Decoder dec(result.value().data(),
                                     result.value().size());
  EXPECT_EQ(dec.base_int(), 7u);
  EXPECT_EQ(dec.base_string().ToStdString(), "hello");
}

TEST(TextToProtoTest, ExtensionWrapperScoped) {
  auto result = Parse(".protozero.test.protos.RealFakeEvent",
                      R"(base_int: 7
                         [protozero.test.protos.BrowserExtension.extension_a] {
                           int_a: 42
                           string_a: "x"
                         })");
  ASSERT_TRUE(result.ok()) << result.status().message();

  pbtest::RealFakeEvent::Decoder dec(result.value().data(),
                                     result.value().size());
  EXPECT_EQ(dec.base_int(), 7u);

  protozero::Field ext_field =
      dec.FindField(pbtest::BrowserExtension::kExtensionAFieldNumber);
  ASSERT_TRUE(ext_field.valid());
  pbtest::SystemA::Decoder ext_dec(ext_field.as_bytes());
  EXPECT_EQ(ext_dec.int_a(), 42u);
  EXPECT_EQ(ext_dec.string_a().ToStdString(), "x");
}

TEST(TextToProtoTest, ExtensionRepeated) {
  auto result = Parse(".protozero.test.protos.RealFakeEvent",
                      R"([protozero.test.protos.BrowserExtension.extension_a] {
                           int_a: 1
                         }
                         [protozero.test.protos.BrowserExtension.extension_b] {
                           int_b: 99
                         })");
  ASSERT_TRUE(result.ok()) << result.status().message();

  pbtest::RealFakeEvent::Decoder dec(result.value().data(),
                                     result.value().size());
  protozero::Field a =
      dec.FindField(pbtest::BrowserExtension::kExtensionAFieldNumber);
  ASSERT_TRUE(a.valid());
  EXPECT_EQ(pbtest::SystemA::Decoder(a.as_bytes()).int_a(), 1u);

  protozero::Field b =
      dec.FindField(pbtest::BrowserExtension::kExtensionBFieldNumber);
  ASSERT_TRUE(b.valid());
  EXPECT_EQ(pbtest::SystemB::Decoder(b.as_bytes()).int_b(), 99u);
}

TEST(TextToProtoTest, ExtensionUnknownName) {
  auto result = Parse(
      ".protozero.test.protos.RealFakeEvent",
      "[protozero.test.protos.BrowserExtension.does_not_exist] { int_a: 1 }");
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("No extension named"),
            std::string::npos)
      << result.status().message();
}

TEST(TextToProtoTest, ExtensionWrongExtendee) {
  // BrowserExtension.extension_a extends RealFakeEvent, not EveryField.
  auto result = Parse(
      ".protozero.test.protos.EveryField",
      "[protozero.test.protos.BrowserExtension.extension_a] { int_a: 1 }");
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("extends"), std::string::npos)
      << result.status().message();
}

TEST(TextToProtoTest, ExtensionScalar) {
  auto result =
      Parse(".protozero.test.protos.RealFakeEvent",
            R"([protozero.test.protos.ScalarBrowserExtension.ext_uint]: 17
         [protozero.test.protos.ScalarBrowserExtension.ext_string]: "abc"
         [protozero.test.protos.ScalarBrowserExtension.ext_bool]: true)");
  ASSERT_TRUE(result.ok()) << result.status().message();

  protozero::ProtoDecoder dec(result.value().data(), result.value().size());
  protozero::Field f_uint = dec.FindField(12);
  ASSERT_TRUE(f_uint.valid());
  EXPECT_EQ(f_uint.as_uint32(), 17u);

  protozero::Field f_str = dec.FindField(13);
  ASSERT_TRUE(f_str.valid());
  EXPECT_EQ(f_str.as_std_string(), "abc");

  protozero::Field f_bool = dec.FindField(14);
  ASSERT_TRUE(f_bool.valid());
  EXPECT_TRUE(f_bool.as_bool());
}

TEST(TextToProtoTest, ExtensionFileScope) {
  auto result = Parse(".protozero.test.protos.RealFakeEvent",
                      "[protozero.test.protos.file_scope_ext]: 123");
  ASSERT_TRUE(result.ok()) << result.status().message();

  protozero::ProtoDecoder dec(result.value().data(), result.value().size());
  protozero::Field f = dec.FindField(15);
  ASSERT_TRUE(f.valid());
  EXPECT_EQ(f.as_uint32(), 123u);
}

TEST(TextToProtoTest, ExtensionEmptyBrackets) {
  auto result = Parse(".protozero.test.protos.RealFakeEvent", "[]: 1");
  EXPECT_FALSE(result.ok());
}

}  // namespace
}  // namespace protozero
