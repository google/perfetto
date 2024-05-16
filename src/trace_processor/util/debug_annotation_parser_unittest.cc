/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/util/debug_annotation_parser.h"

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"
#include "protos/perfetto/trace/track_event/source_location.pbzero.h"
#include "src/protozero/test/example_proto/test_messages.pbzero.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_builder.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/test_messages.descriptor.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/interned_message_view.h"
#include "src/trace_processor/util/proto_to_args_parser.h"
#include "test/gtest_and_gmock.h"

#include <sstream>

namespace perfetto {
namespace trace_processor {
namespace util {
namespace {

base::Status ParseDebugAnnotation(
    DebugAnnotationParser& parser,
    protozero::HeapBuffered<protos::pbzero::DebugAnnotation>& msg,
    ProtoToArgsParser::Delegate& delegate) {
  std::vector<uint8_t> data = msg.SerializeAsArray();
  return parser.Parse(protozero::ConstBytes{data.data(), data.size()},
                      delegate);
}

class DebugAnnotationParserTest : public ::testing::Test,
                                  public ProtoToArgsParser::Delegate {
 protected:
  DebugAnnotationParserTest() { context_.storage.reset(new TraceStorage()); }

  const std::vector<std::string>& args() const { return args_; }

  void InternMessage(uint32_t field_id, TraceBlobView message) {
    state_builder_.InternMessage(field_id, std::move(message));
  }

 private:
  using Key = ProtoToArgsParser::Key;

  void AddInteger(const Key& key, int64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddUnsignedInteger(const Key& key, uint64_t value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddString(const Key& key, const protozero::ConstChars& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value.ToStdString();
    args_.push_back(ss.str());
  }

  void AddString(const Key& key, const std::string& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddDouble(const Key& key, double value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << value;
    args_.push_back(ss.str());
  }

  void AddPointer(const Key& key, const void* value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << std::hex
       << reinterpret_cast<uintptr_t>(value) << std::dec;
    args_.push_back(ss.str());
  }

  void AddBoolean(const Key& key, bool value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << (value ? "true" : "false");
    args_.push_back(ss.str());
  }

  bool AddJson(const Key& key, const protozero::ConstChars& value) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " " << std::hex
       << value.ToStdString() << std::dec;
    args_.push_back(ss.str());
    return true;
  }

  void AddNull(const Key& key) override {
    std::stringstream ss;
    ss << key.flat_key << " " << key.key << " [NULL]";
    args_.push_back(ss.str());
  }

  size_t GetArrayEntryIndex(const std::string& array_key) final {
    return array_indices_[array_key];
  }

  size_t IncrementArrayEntryIndex(const std::string& array_key) final {
    return ++array_indices_[array_key];
  }

  InternedMessageView* GetInternedMessageView(uint32_t field_id,
                                              uint64_t iid) override {
    return state_builder_.current_generation()->GetInternedMessageView(field_id,
                                                                       iid);
  }

  PacketSequenceStateGeneration* seq_state() final {
    return state_builder_.current_generation().get();
  }

  std::vector<std::string> args_;
  std::map<std::string, size_t> array_indices_;

  TraceProcessorContext context_;
  PacketSequenceStateBuilder state_builder_{&context_};
};

// This test checks that in when an array is nested inside a dict which is
// nested inside an array which is nested inside a dict, flat keys and non-flat
// keys are parsed correctly.
TEST_F(DebugAnnotationParserTest, DeeplyNestedDictsAndArrays) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;

  msg->set_name("root");
  auto* dict1 = msg->add_dict_entries();
  dict1->set_name("k1");
  auto* array1 = dict1->add_array_values();
  auto* dict2 = array1->add_dict_entries();
  dict2->set_name("k2");
  auto* array2 = dict2->add_array_values();
  array2->set_int_value(42);

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  EXPECT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  ProtoToArgsParser args_parser(pool);
  DebugAnnotationParser parser(args_parser);

  status = ParseDebugAnnotation(parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "DebugAnnotationParser::Parse failed with error: "
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre("root.k1.k2 root.k1[0].k2[0] 42"));
}

// This test checks that array indexes are correctly merged across messages.
TEST_F(DebugAnnotationParserTest, MergeArrays) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg1;
  msg1->set_name("root");
  auto* item1 = msg1->add_array_values();
  item1->set_int_value(1);

  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg2;
  msg2->set_name("root");
  auto* item2 = msg1->add_array_values();
  item2->set_int_value(2);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);
  DebugAnnotationParser parser(args_parser);

  base::Status status = ParseDebugAnnotation(parser, msg1, *this);
  EXPECT_TRUE(status.ok()) << "DebugAnnotationParser::Parse failed with error: "
                           << status.message();

  status = ParseDebugAnnotation(parser, msg2, *this);
  EXPECT_TRUE(status.ok()) << "DebugAnnotationParser::Parse failed with error: "
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre("root root[0] 1", "root root[1] 2"));
}

// This test checks that nested empty dictionaries / arrays do not cause array
// index to be incremented.
TEST_F(DebugAnnotationParserTest, EmptyArrayIndexIsSkipped) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");

  msg->add_array_values()->set_int_value(1);

  // Empty item.
  msg->add_array_values();

  msg->add_array_values()->set_int_value(3);

  // Empty dict.
  msg->add_array_values()->add_dict_entries()->set_name("key1");

  auto* nested_dict_entry = msg->add_array_values()->add_dict_entries();
  nested_dict_entry->set_name("key2");
  nested_dict_entry->set_string_value("value");

  msg->add_array_values()->set_int_value(5);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);
  DebugAnnotationParser parser(args_parser);

  base::Status status = ParseDebugAnnotation(parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "DebugAnnotationParser::Parse failed with error: "
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre("root root[0] 1", "root root[1] 3",
                                           "root.key2 root[3].key2 value",
                                           "root root[4] 5"));
}

TEST_F(DebugAnnotationParserTest, NestedArrays) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");
  auto* item1 = msg->add_array_values();
  item1->add_array_values()->set_int_value(1);
  item1->add_array_values()->set_int_value(2);
  auto* item2 = msg->add_array_values();
  item2->add_array_values()->set_int_value(3);
  item2->add_array_values()->set_int_value(4);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);
  DebugAnnotationParser parser(args_parser);

  base::Status status = ParseDebugAnnotation(parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "DebugAnnotationParser::Parse failed with error: "
                           << status.message();

  EXPECT_THAT(args(),
              testing::ElementsAre("root root[0][0] 1", "root root[0][1] 2",
                                   "root root[1][0] 3", "root root[1][1] 4"));
}

TEST_F(DebugAnnotationParserTest, TypedMessageInsideUntyped) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");

  protozero::HeapBuffered<protozero::test::protos::pbzero::EveryField> message;
  message->set_field_string("value");

  msg->set_proto_type_name(message->GetName());
  msg->set_proto_value(message.SerializeAsString());

  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTestMessagesDescriptor.data(),
                                              kTestMessagesDescriptor.size());
  EXPECT_TRUE(status.ok()) << "Failed to parse kTestMessagesDescriptor: "
                           << status.message();

  ProtoToArgsParser args_parser(pool);
  DebugAnnotationParser parser(args_parser);

  status = ParseDebugAnnotation(parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "DebugAnnotationParser::Parse failed with error: "
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre(
                          "root.field_string root.field_string value"));
}

TEST_F(DebugAnnotationParserTest, InternedString) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> msg;
  msg->set_name("root");

  protozero::HeapBuffered<protos::pbzero::InternedString> string;
  string->set_iid(1);
  string->set_str("foo");
  std::vector<uint8_t> data_serialized = string.SerializeAsArray();

  InternMessage(
      protos::pbzero::InternedData::kDebugAnnotationStringValuesFieldNumber,
      TraceBlobView(
          TraceBlob::CopyFrom(data_serialized.data(), data_serialized.size())));

  msg->set_string_value_iid(1);

  DescriptorPool pool;
  ProtoToArgsParser args_parser(pool);
  DebugAnnotationParser parser(args_parser);

  auto status = ParseDebugAnnotation(parser, msg, *this);
  EXPECT_TRUE(status.ok()) << "DebugAnnotationParser::Parse failed with error: "
                           << status.message();

  EXPECT_THAT(args(), testing::ElementsAre("root root foo"));
}

}  // namespace
}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto
