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

#include "perfetto/tracing/traced_value.h"

#include <array>
#include <deque>
#include <forward_list>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <unordered_map>
#include <unordered_set>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/debug_annotation.h"
#include "perfetto/tracing/track_event.h"
#include "protos/perfetto/trace/track_event/debug_annotation.gen.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pb.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {

namespace {

void WriteAsJSON(const protos::DebugAnnotation::NestedValue& value,
                 std::stringstream& ss) {
  if (value.nested_type() ==
      protos::DebugAnnotation_NestedValue_NestedType_DICT) {
    ss << "{";
    for (int i = 0; i < value.dict_keys_size() && i < value.dict_values_size();
         ++i) {
      if (i > 0)
        ss << ",";
      ss << value.dict_keys(i);
      ss << ":";
      WriteAsJSON(value.dict_values(i), ss);
    }
    ss << "}";
    return;
  } else if (value.nested_type() ==
             protos::DebugAnnotation_NestedValue_NestedType_ARRAY) {
    ss << "[";
    for (int i = 0; i < value.array_values_size(); ++i) {
      if (i > 0)
        ss << ",";
      WriteAsJSON(value.array_values(i), ss);
    }
    ss << "]";
    return;
  } else if (value.has_int_value()) {
    ss << value.int_value();
    return;
  } else if (value.has_double_value()) {
    ss << value.double_value();
    return;
  } else if (value.has_bool_value()) {
    ss << static_cast<bool>(value.bool_value());
    return;
  } else if (value.has_string_value()) {
    ss << value.string_value();
    return;
  }
}

void WriteAsJSON(const protos::DebugAnnotation& value, std::stringstream& ss) {
  if (value.has_bool_value()) {
    ss << static_cast<bool>(value.bool_value());
    return;
  } else if (value.has_uint_value()) {
    ss << value.uint_value();
    return;
  } else if (value.has_int_value()) {
    ss << value.int_value();
    return;
  } else if (value.has_double_value()) {
    ss << value.double_value();
    return;
  } else if (value.has_string_value()) {
    ss << value.string_value();
    return;
  } else if (value.has_pointer_value()) {
    ss << value.pointer_value();
    return;
  } else if (value.has_nested_value()) {
    WriteAsJSON(value.nested_value(), ss);
    return;
  } else if (value.has_legacy_json_value()) {
    ss << value.legacy_json_value();
    return;
  }
}

std::string MessageToJSON(const std::string& data) {
  std::stringstream ss;
  protos::DebugAnnotation result;
  result.ParseFromString(data);
  WriteAsJSON(result, ss);
  return ss.str();
}

}  // namespace

TEST(TracedValueTest, FlatDictionary_Explicit) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
  {
    auto dict = TracedValue::CreateForTest(message.get()).WriteDictionary();
    dict.AddItem("bool").WriteBoolean(true);
    dict.AddItem("double").WriteDouble(0.0);
    dict.AddItem("int").WriteInt64(2014);
    dict.AddItem("string").WriteString("string");
    dict.AddItem("ptr").WritePointer(reinterpret_cast<void*>(0x1234));
  }
  EXPECT_EQ("{bool:1,double:0,int:2014,string:string,ptr:4660}",
            MessageToJSON(message.SerializeAsString()));
}

TEST(TracedValueTest, Hierarchy_Explicit) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
  {
    auto root_dict =
        TracedValue::CreateForTest(message.get()).WriteDictionary();
    {
      auto array = root_dict.AddItem("a1").WriteArray();
      array.AppendItem().WriteInt64(1);
      array.AppendItem().WriteBoolean(true);
      {
        auto dict = array.AppendItem().WriteDictionary();
        dict.AddItem("i2").WriteInt64(3);
      }
    }
    root_dict.AddItem("b0").WriteBoolean(true);
    root_dict.AddItem("d0").WriteDouble(0.0);
    {
      auto dict1 = root_dict.AddItem("dict1").WriteDictionary();
      {
        auto dict2 = dict1.AddItem("dict2").WriteDictionary();
        dict2.AddItem("b2").WriteBoolean(false);
      }
      dict1.AddItem("i1").WriteInt64(2014);
      dict1.AddItem("s1").WriteString("foo");
    }
    root_dict.AddItem("i0").WriteInt64(2014);
    root_dict.AddItem("s0").WriteString("foo");
  }

  EXPECT_EQ(
      "{"
      "a1:[1,1,{i2:3}],"
      "b0:1,"
      "d0:0,"
      "dict1:{dict2:{b2:0},i1:2014,s1:foo},"
      "i0:2014,"
      "s0:foo}",
      MessageToJSON(message.SerializeAsString()));
}

}  // namespace perfetto
