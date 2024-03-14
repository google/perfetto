/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "perfetto/protozero/scattered_heap_buffer.h"

#include "src/trace_redaction/proto_util.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/config/test_config.gen.h"
#include "protos/perfetto/config/test_config.pbzero.h"
#include "protos/perfetto/trace/trace_packet.gen.h"

namespace perfetto::trace_redaction {

namespace proto_util {

class ProtoUtilTest : public testing::Test {
 protected:
  void Reserialize(const protos::gen::TestConfig_DummyFields& fields) {
    // Serialize the object and then deserialize it with proto decoder. This is
    // needed to get the fields.
    auto serialized = fields.SerializeAsString();
    protozero::ProtoDecoder decoder(serialized);

    protozero::HeapBuffered<protos::pbzero::TestConfig_DummyFields> message;

    for (auto field = decoder.ReadField(); field.valid();
         field = decoder.ReadField()) {
      AppendField(field, message.get());
    }

    auto reserialized = message.SerializeAsString();

    ASSERT_EQ(serialized, reserialized);
  }
};

class ProtoUtilUint32Test : public ProtoUtilTest,
                            public testing::WithParamInterface<uint32_t> {};
class ProtoUtilUint64Test : public ProtoUtilTest,
                            public testing::WithParamInterface<uint64_t> {};
class ProtoUtilInt32Test : public ProtoUtilTest,
                           public testing::WithParamInterface<int32_t> {};
class ProtoUtilInt64Test : public ProtoUtilTest,
                           public testing::WithParamInterface<int64_t> {};
class ProtoUtilFixed32Test : public ProtoUtilTest,
                             public testing::WithParamInterface<uint32_t> {};
class ProtoUtilFixed64Test : public ProtoUtilTest,
                             public testing::WithParamInterface<uint64_t> {};
class ProtoUtilSfixed32Test : public ProtoUtilTest,
                              public testing::WithParamInterface<int32_t> {};
class ProtoUtilSfixed64Test : public ProtoUtilTest,
                              public testing::WithParamInterface<int64_t> {};
class ProtoUtilDoubleTest : public ProtoUtilTest,
                            public testing::WithParamInterface<double> {};
class ProtoUtilFloatTest : public ProtoUtilTest,
                           public testing::WithParamInterface<float> {};
class ProtoUtilSint32Test : public ProtoUtilTest,
                            public testing::WithParamInterface<int32_t> {};
class ProtoUtilSint64Test : public ProtoUtilTest,
                            public testing::WithParamInterface<int64_t> {};
class ProtoUtilStringTest : public ProtoUtilTest,
                            public testing::WithParamInterface<std::string> {};
class ProtoUtilBytesTest : public ProtoUtilTest,
                           public testing::WithParamInterface<std::string> {};

TEST_P(ProtoUtilUint32Test, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_uint32(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilUint32Test,
                         testing::Values(std::numeric_limits<uint32_t>::min(),
                                         0,
                                         0xFAAAAAAA,
                                         std::numeric_limits<uint32_t>::max()));

TEST_P(ProtoUtilUint64Test, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_uint64(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilUint64Test,
                         testing::Values(std::numeric_limits<uint64_t>::min(),
                                         0,
                                         0xFAAAAAAAAAAAAAAA,
                                         std::numeric_limits<uint64_t>::max()));

TEST_P(ProtoUtilInt32Test, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_int32(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilInt32Test,
                         testing::Values(std::numeric_limits<int32_t>::min(),
                                         0xFAAAAAAA,
                                         0,
                                         0x0AAAAAAA,
                                         std::numeric_limits<int32_t>::max()));

TEST_P(ProtoUtilInt64Test, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_int64(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilInt64Test,
                         testing::Values(std::numeric_limits<int64_t>::min(),
                                         0xFAAAAAAAAAAAAAAA,
                                         0,
                                         0x0AAAAAAAAAAAAAAA,
                                         std::numeric_limits<int64_t>::max()));

TEST_P(ProtoUtilFixed32Test, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_fixed32(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilFixed32Test,
                         testing::Values(std::numeric_limits<uint32_t>::min(),
                                         0,
                                         0xFAAAAAAAAAAAAAAA,
                                         std::numeric_limits<uint32_t>::max()));

TEST_P(ProtoUtilSfixed32Test, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_sfixed32(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilSfixed32Test,
                         testing::Values(std::numeric_limits<int32_t>::min(),
                                         0xFAAAAAAA,
                                         0,
                                         0x0AAAAAAA,
                                         std::numeric_limits<int32_t>::max()));

TEST_P(ProtoUtilDoubleTest, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_double(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(
    Reserialize,
    ProtoUtilDoubleTest,
    testing::Values(std::numeric_limits<double>::min(),
                    0.0,
                    1.0,
                    std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::max()));

TEST_P(ProtoUtilFloatTest, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_float(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilFloatTest,
                         testing::Values(std::numeric_limits<float>::min(),
                                         0.0f,
                                         1.0f,
                                         std::numeric_limits<float>::infinity(),
                                         std::numeric_limits<float>::max()));

TEST_P(ProtoUtilSint64Test, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_sint64(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilSint64Test,
                         testing::Values(std::numeric_limits<int64_t>::min(),
                                         0xFAAAAAAAAAAAAAAA,
                                         0,
                                         0x0AAAAAAAAAAAAAAA,
                                         std::numeric_limits<int64_t>::max()));

TEST_P(ProtoUtilSint32Test, FullDomain) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_sint32(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilSint32Test,
                         testing::Values(std::numeric_limits<int32_t>::min(),
                                         0xFAAAAAAA,
                                         0,
                                         0x0AAAAAAA,
                                         std::numeric_limits<int32_t>::max()));

TEST_P(ProtoUtilStringTest, Various) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_string(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilStringTest,
                         testing::Values("",
                                         "a",
                                         "abcdefghijklmonpqrstuvwxyz",
                                         std::string(1024, 'a')));

TEST_P(ProtoUtilBytesTest, Various) {
  auto value = GetParam();

  protos::gen::TestConfig_DummyFields fields;
  fields.set_field_bytes(value);
  Reserialize(fields);
}

INSTANTIATE_TEST_SUITE_P(Reserialize,
                         ProtoUtilBytesTest,
                         testing::Values("",
                                         "a",
                                         "abcdefghijklmonpqrstuvwxyz",
                                         std::string(1024, 'a')));

}  // namespace proto_util

}  // namespace perfetto::trace_redaction
