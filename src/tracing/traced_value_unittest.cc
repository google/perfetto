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

#include "perfetto/base/template_util.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/debug_annotation.h"
#include "perfetto/tracing/track_event.h"
#include "protos/perfetto/trace/track_event/debug_annotation.gen.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pb.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {

// static asserts checking for conversion support for known types.

#define ASSERT_TYPE_SUPPORTED(T)                           \
  static_assert(check_traced_value_support<T>::value, ""); \
  static_assert(internal::has_traced_value_support<T>::value, "")

#define ASSERT_TYPE_NOT_SUPPORTED(T) \
  static_assert(!internal::has_traced_value_support<T>::value, "")

struct NonSupportedType {};

ASSERT_TYPE_SUPPORTED(bool);

ASSERT_TYPE_NOT_SUPPORTED(NonSupportedType);

// Integer types.
ASSERT_TYPE_SUPPORTED(short int);
ASSERT_TYPE_SUPPORTED(unsigned short int);
ASSERT_TYPE_SUPPORTED(int);
ASSERT_TYPE_SUPPORTED(unsigned int);
ASSERT_TYPE_SUPPORTED(long int);
ASSERT_TYPE_SUPPORTED(unsigned long int);
ASSERT_TYPE_SUPPORTED(long long int);
ASSERT_TYPE_SUPPORTED(unsigned long long int);

// References and const references types.
ASSERT_TYPE_SUPPORTED(int&);
ASSERT_TYPE_SUPPORTED(const int&);
ASSERT_TYPE_NOT_SUPPORTED(NonSupportedType&);
ASSERT_TYPE_NOT_SUPPORTED(const NonSupportedType&);

// Character types.
ASSERT_TYPE_SUPPORTED(signed char);
ASSERT_TYPE_SUPPORTED(unsigned char);
ASSERT_TYPE_SUPPORTED(char);
ASSERT_TYPE_SUPPORTED(wchar_t);

// Float types.
ASSERT_TYPE_SUPPORTED(float);
ASSERT_TYPE_SUPPORTED(double);
ASSERT_TYPE_SUPPORTED(long double);

// Strings.
ASSERT_TYPE_SUPPORTED(const char*);
ASSERT_TYPE_SUPPORTED(const char[]);
ASSERT_TYPE_SUPPORTED(const char[2]);
ASSERT_TYPE_SUPPORTED(std::string);

// Pointers.
ASSERT_TYPE_SUPPORTED(int*);
ASSERT_TYPE_SUPPORTED(const int*);
ASSERT_TYPE_SUPPORTED(void*);
ASSERT_TYPE_SUPPORTED(const void*);
ASSERT_TYPE_SUPPORTED(nullptr_t);
ASSERT_TYPE_NOT_SUPPORTED(NonSupportedType*);
ASSERT_TYPE_NOT_SUPPORTED(const NonSupportedType*);

// Arrays.
ASSERT_TYPE_NOT_SUPPORTED(int[]);
ASSERT_TYPE_NOT_SUPPORTED(const int[]);
ASSERT_TYPE_NOT_SUPPORTED(NonSupportedType[]);
ASSERT_TYPE_NOT_SUPPORTED(const NonSupportedType[]);
ASSERT_TYPE_SUPPORTED(int (&)[3]);
ASSERT_TYPE_SUPPORTED(const int (&)[3]);
ASSERT_TYPE_NOT_SUPPORTED(NonSupportedType (&)[3]);
ASSERT_TYPE_NOT_SUPPORTED(const NonSupportedType (&)[3]);

// STL containers.
ASSERT_TYPE_SUPPORTED(std::vector<int>);
ASSERT_TYPE_NOT_SUPPORTED(std::vector<NonSupportedType>);

using array_int_t = std::array<int, 4>;
ASSERT_TYPE_SUPPORTED(array_int_t);
ASSERT_TYPE_SUPPORTED(std::deque<int>);
ASSERT_TYPE_SUPPORTED(std::forward_list<int>);
ASSERT_TYPE_SUPPORTED(std::list<int>);
ASSERT_TYPE_NOT_SUPPORTED(std::stack<int>);
ASSERT_TYPE_NOT_SUPPORTED(std::queue<int>);
ASSERT_TYPE_NOT_SUPPORTED(std::priority_queue<int>);
ASSERT_TYPE_SUPPORTED(std::set<int>);
ASSERT_TYPE_SUPPORTED(std::multiset<int>);
using map_int_int_t = std::map<int, int>;
ASSERT_TYPE_NOT_SUPPORTED(map_int_int_t);
using multimap_int_int_t = std::multimap<int, int>;
ASSERT_TYPE_NOT_SUPPORTED(multimap_int_int_t);
ASSERT_TYPE_SUPPORTED(std::unordered_set<int>);
ASSERT_TYPE_SUPPORTED(std::unordered_multiset<int>);
using unordered_map_int_int_t = std::unordered_map<int, int>;
ASSERT_TYPE_NOT_SUPPORTED(unordered_map_int_int_t);
using unordered_multimap_int_int_t = std::unordered_multimap<int, int>;
ASSERT_TYPE_NOT_SUPPORTED(unordered_multimap_int_int_t);

// unique_ptr.
ASSERT_TYPE_SUPPORTED(std::unique_ptr<int>);
ASSERT_TYPE_NOT_SUPPORTED(std::unique_ptr<NonSupportedType>);

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
    if (value.bool_value()) {
      ss << "true";
    } else {
      ss << "false";
    }
    return;
  } else if (value.has_string_value()) {
    ss << value.string_value();
    return;
  }
}

void WriteAsJSON(const protos::DebugAnnotation& value, std::stringstream& ss) {
  if (value.has_bool_value()) {
    if (value.bool_value()) {
      ss << "true";
    } else {
      ss << "false";
    }
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
    // Printing pointer values via ostream is really platform-specific, so do
    // not try to convert it to void* before printing.
    ss << "0x" << std::hex << value.pointer_value() << std::dec;
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
    auto dict =
        internal::CreateTracedValueFromProto(message.get()).WriteDictionary();
    dict.AddItem("bool").WriteBoolean(true);
    dict.AddItem("double").WriteDouble(0.0);
    dict.AddItem("int").WriteInt64(2014);
    dict.AddItem("string").WriteString("string");
    dict.AddItem("ptr").WritePointer(reinterpret_cast<void*>(0x1234));
  }
  // TODO(altimin): Nested pointers are recorded as ints due to proto
  // limitation. Fix after sorting out the NestedValue.
  EXPECT_EQ("{bool:true,double:0,int:2014,string:string,ptr:4660}",
            MessageToJSON(message.SerializeAsString()));
}

TEST(TracedValueTest, FlatDictionary_Short) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
  {
    auto dict =
        internal::CreateTracedValueFromProto(message.get()).WriteDictionary();
    dict.Add("bool", true);
    dict.Add("double", 0.0);
    dict.Add("int", 2014);
    dict.Add("string", "string");
    dict.Add("ptr", reinterpret_cast<void*>(0x1234));
  }
  // TODO(altimin): Nested pointers are recorded as ints due to proto
  // limitation. Fix after sorting out the NestedValue.
  EXPECT_EQ("{bool:true,double:0,int:2014,string:string,ptr:4660}",
            MessageToJSON(message.SerializeAsString()));
}

TEST(TracedValueTest, Hierarchy_Explicit) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
  {
    auto root_dict =
        internal::CreateTracedValueFromProto(message.get()).WriteDictionary();
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
      "a1:[1,true,{i2:3}],"
      "b0:true,"
      "d0:0,"
      "dict1:{dict2:{b2:false},i1:2014,s1:foo},"
      "i0:2014,"
      "s0:foo}",
      MessageToJSON(message.SerializeAsString()));
}

TEST(TracedValueTest, Hierarchy_Short) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
  {
    auto root_dict =
        internal::CreateTracedValueFromProto(message.get()).WriteDictionary();
    {
      auto array = root_dict.AddArray("a1");
      array.Append(1);
      array.Append(true);
      {
        auto dict = array.AppendDictionary();
        dict.Add("i2", 3);
      }
    }
    root_dict.Add("b0", true);
    root_dict.Add("d0", 0.0);
    {
      auto dict1 = root_dict.AddDictionary("dict1");
      {
        auto dict2 = dict1.AddDictionary("dict2");
        dict2.Add("b2", false);
      }
      dict1.Add("i1", 2014);
      dict1.Add("s1", "foo");
    }
    root_dict.Add("i0", 2014);
    root_dict.Add("s0", "foo");
  }

  EXPECT_EQ(
      "{"
      "a1:[1,true,{i2:3}],"
      "b0:true,"
      "d0:0,"
      "dict1:{dict2:{b2:false},i1:2014,s1:foo},"
      "i0:2014,"
      "s0:foo}",
      MessageToJSON(message.SerializeAsString()));
}

namespace {

class HasConvertorMember {
 public:
  void WriteIntoTracedValue(TracedValue context) const {
    auto dict = std::move(context).WriteDictionary();
    dict.Add("int", 42);
    dict.Add("bool", false);
  }
};

class HasExternalConvertor {};

class HasAllConversionMethods {
 public:
  void WriteIntoTracedValue(TracedValue context) const {
    std::move(context).WriteString("T::WriteIntoTracedValue");
  }

  void operator()(TracedValue context) const {
    std::move(context).WriteString("T::()");
  }
};

class NoConversions {};

class HasConstWriteMember {
 public:
  void WriteIntoTracedValue(TracedValue context) const {
    std::move(context).WriteString("T::WriteIntoTracedValue const");
  }
};

class HasNonConstWriteMember {
 public:
  void WriteIntoTracedValue(TracedValue context) {
    std::move(context).WriteString("T::WriteIntoTracedValue");
  }
};

class HasConstAndNonConstWriteMember {
 public:
  void WriteIntoTracedValue(TracedValue context) {
    std::move(context).WriteString("T::WriteIntoTracedValue");
  }

  void WriteIntoTracedValue(TracedValue context) const {
    std::move(context).WriteString("T::WriteIntoTracedValue const");
  }
};

}  // namespace

template <>
struct TraceFormatTraits<HasExternalConvertor> {
  static void WriteIntoTracedValue(TracedValue context,
                                   const HasExternalConvertor&) {
    std::move(context).WriteString("TraceFormatTraits::WriteIntoTracedValue");
  }
};

template <>
struct TraceFormatTraits<HasAllConversionMethods> {
  static void WriteIntoTracedValue(TracedValue context,
                                   const HasAllConversionMethods&) {
    std::move(context).WriteString("TraceFormatTraits::WriteIntoTracedValue");
  }
};

template <typename T>
std::string ToString(T&& value) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
  WriteIntoTracedValue(internal::CreateTracedValueFromProto(message.get()),
                       std::forward<T>(value));
  return MessageToJSON(message.SerializeAsString());
}

template <typename T>
std::string ToStringWithFallback(T&& value, const std::string& fallback) {
  protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
  WriteIntoTracedValueWithFallback(
      internal::CreateTracedValueFromProto(message.get()),
      std::forward<T>(value), fallback);
  return MessageToJSON(message.SerializeAsString());
}

ASSERT_TYPE_SUPPORTED(HasConvertorMember);
ASSERT_TYPE_SUPPORTED(HasExternalConvertor);
ASSERT_TYPE_SUPPORTED(HasAllConversionMethods);

ASSERT_TYPE_SUPPORTED(HasConstWriteMember);
ASSERT_TYPE_SUPPORTED(HasConstWriteMember&);
ASSERT_TYPE_SUPPORTED(HasConstWriteMember*);
ASSERT_TYPE_SUPPORTED(std::unique_ptr<HasConstWriteMember>);
ASSERT_TYPE_SUPPORTED(std::vector<HasConstWriteMember>);
ASSERT_TYPE_SUPPORTED(const HasConstWriteMember);
ASSERT_TYPE_SUPPORTED(const HasConstWriteMember&);
ASSERT_TYPE_SUPPORTED(const HasConstWriteMember*);
ASSERT_TYPE_SUPPORTED(std::unique_ptr<const HasConstWriteMember>);
ASSERT_TYPE_SUPPORTED(const std::vector<HasConstWriteMember>);
ASSERT_TYPE_SUPPORTED(std::vector<const HasConstWriteMember*>);

ASSERT_TYPE_SUPPORTED(HasNonConstWriteMember);
ASSERT_TYPE_SUPPORTED(HasNonConstWriteMember&);
ASSERT_TYPE_SUPPORTED(HasNonConstWriteMember*);
ASSERT_TYPE_SUPPORTED(std::unique_ptr<HasNonConstWriteMember>);
ASSERT_TYPE_SUPPORTED(std::vector<HasNonConstWriteMember>);
ASSERT_TYPE_NOT_SUPPORTED(const HasNonConstWriteMember);
ASSERT_TYPE_NOT_SUPPORTED(const HasNonConstWriteMember&);
ASSERT_TYPE_NOT_SUPPORTED(const HasNonConstWriteMember*);
ASSERT_TYPE_NOT_SUPPORTED(std::unique_ptr<const HasNonConstWriteMember>);
ASSERT_TYPE_NOT_SUPPORTED(const std::vector<HasNonConstWriteMember>);
ASSERT_TYPE_NOT_SUPPORTED(std::vector<const HasNonConstWriteMember*>);

ASSERT_TYPE_SUPPORTED(HasConstAndNonConstWriteMember);
ASSERT_TYPE_SUPPORTED(HasConstAndNonConstWriteMember&);
ASSERT_TYPE_SUPPORTED(HasConstAndNonConstWriteMember*);
ASSERT_TYPE_SUPPORTED(std::unique_ptr<HasConstAndNonConstWriteMember>);
ASSERT_TYPE_SUPPORTED(const HasConstAndNonConstWriteMember);
ASSERT_TYPE_SUPPORTED(const HasConstAndNonConstWriteMember&);
ASSERT_TYPE_SUPPORTED(const HasConstAndNonConstWriteMember*);
ASSERT_TYPE_SUPPORTED(std::unique_ptr<const HasConstAndNonConstWriteMember*>);

TEST(TracedValueTest, UserDefinedConvertors) {
  HasConvertorMember value1;
  EXPECT_EQ(ToString(value1), "{int:42,bool:false}");
  EXPECT_EQ(ToString(&value1), "{int:42,bool:false}");

  HasExternalConvertor value2;
  EXPECT_EQ(ToString(value2), "TraceFormatTraits::WriteIntoTracedValue");
  EXPECT_EQ(ToString(&value2), "TraceFormatTraits::WriteIntoTracedValue");

  HasAllConversionMethods value3;
  EXPECT_EQ(ToString(value3), "T::WriteIntoTracedValue");
  EXPECT_EQ(ToString(&value3), "T::WriteIntoTracedValue");
}

TEST(TracedValueTest, WriteAsLambda) {
  EXPECT_EQ("42", ToString([&](TracedValue context) {
              std::move(context).WriteInt64(42);
            }));
}

#if PERFETTO_DCHECK_IS_ON()
// This death test makes sense only when dchecks are enabled.
TEST(TracedValueTest, FailOnIncorrectUsage) {
  // A new call to AddItem is not allowed before the previous result is
  // consumed.
  EXPECT_DEATH(

      {
        protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
        auto dict = internal::CreateTracedValueFromProto(message.get())
                        .WriteDictionary();
        auto scope1 = dict.AddItem("key1");
        auto scope2 = dict.AddItem("key2");
        std::move(scope1).WriteInt64(1);
        std::move(scope2).WriteInt64(2);
      },
      "");

  // A new call to AppendItem is not allowed before the previous result is
  // consumed.
  EXPECT_DEATH(
      {
        protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
        auto array =
            internal::CreateTracedValueFromProto(message.get()).WriteArray();
        auto scope1 = array.AppendItem();
        auto scope2 = array.AppendItem();
        std::move(scope1).WriteInt64(1);
        std::move(scope2).WriteInt64(2);
      },
      "");

  // Writing to parent scope is not allowed.
  EXPECT_DEATH(
      {
        protozero::HeapBuffered<protos::pbzero::DebugAnnotation> message;
        auto outer_dict = internal::CreateTracedValueFromProto(message.get())
                              .WriteDictionary();
        {
          auto inner_dict = outer_dict.AddDictionary("inner");
          outer_dict.Add("key", "value");
        }
      },
      "");
}
#endif  // PERFETTO_DCHECK_IS_ON()

TEST(TracedValueTest, PrimitiveTypesSupport) {
  EXPECT_EQ("0x0", ToString(nullptr));
  EXPECT_EQ("0x1", ToString(reinterpret_cast<void*>(1)));

  const int int_value = 1;
  EXPECT_EQ("1", ToString(int_value));
  EXPECT_EQ("1", ToString(&int_value));

  EXPECT_EQ("1.5", ToString(1.5));
  EXPECT_EQ("true", ToString(true));
  EXPECT_EQ("foo", ToString("foo"));
  EXPECT_EQ("bar", ToString(std::string("bar")));
}

TEST(TracedValueTest, UniquePtrSupport) {
  std::unique_ptr<int> value1;
  EXPECT_EQ("0x0", ToString(value1));

  std::unique_ptr<int> value2(new int(4));
  EXPECT_EQ("4", ToString(value2));
}

namespace {

enum OldStyleEnum { kFoo, kBar };

enum class NewStyleEnum { kValue1, kValue2 };

enum class EnumWithPrettyPrint { kValue1, kValue2 };

}  // namespace

template <>
struct TraceFormatTraits<EnumWithPrettyPrint> {
  static void WriteIntoTracedValue(TracedValue context,
                                   EnumWithPrettyPrint value) {
    switch (value) {
      case EnumWithPrettyPrint::kValue1:
        std::move(context).WriteString("value1");
        return;
      case EnumWithPrettyPrint::kValue2:
        std::move(context).WriteString("value2");
        return;
    }
  }
};

TEST(TracedValueTest, EnumSupport) {
  EXPECT_EQ(ToString(kFoo), "0");
  EXPECT_EQ(ToString(NewStyleEnum::kValue2), "1");
  EXPECT_EQ(ToString(EnumWithPrettyPrint::kValue2), "value2");
}

TEST(TracedValueTest, ContainerSupport) {
  std::vector<std::list<int>> value1{{1, 2}, {3, 4}};
  EXPECT_EQ("[[1,2],[3,4]]", ToString(value1));
}

TEST(TracedValueTest, WriteWithFallback) {
  EXPECT_EQ("1", ToStringWithFallback(1, "fallback"));
  EXPECT_EQ("true", ToStringWithFallback(true, "fallback"));
  EXPECT_EQ("fallback", ToStringWithFallback(NonSupportedType(), "fallback"));
}

TEST(TracedValueTest, ConstAndNotConstSupport) {
  {
    HasConstWriteMember value;
    EXPECT_EQ("T::WriteIntoTracedValue const", ToString(value));
    EXPECT_EQ("T::WriteIntoTracedValue const", ToString(&value));

    std::vector<HasConstWriteMember> arr(1, value);
    EXPECT_EQ("[T::WriteIntoTracedValue const]", ToString(arr));
  }

  {
    const HasConstWriteMember value;
    EXPECT_EQ("T::WriteIntoTracedValue const", ToString(value));
    EXPECT_EQ("T::WriteIntoTracedValue const", ToString(&value));

    const std::vector<HasConstWriteMember> arr(1, value);
    EXPECT_EQ("[T::WriteIntoTracedValue const]", ToString(arr));
  }

  {
    HasNonConstWriteMember value;
    EXPECT_EQ("T::WriteIntoTracedValue", ToString(value));
    EXPECT_EQ("T::WriteIntoTracedValue", ToString(&value));

    std::vector<HasNonConstWriteMember> arr(1, value);
    EXPECT_EQ("[T::WriteIntoTracedValue]", ToString(arr));
  }

  {
    HasConstAndNonConstWriteMember value;
    EXPECT_EQ("T::WriteIntoTracedValue", ToString(value));
    EXPECT_EQ("T::WriteIntoTracedValue", ToString(&value));

    std::vector<HasConstAndNonConstWriteMember> arr(1, value);
    EXPECT_EQ("[T::WriteIntoTracedValue]", ToString(arr));
  }

  {
    const HasConstAndNonConstWriteMember value;
    EXPECT_EQ("T::WriteIntoTracedValue const", ToString(value));
    EXPECT_EQ("T::WriteIntoTracedValue const", ToString(&value));

    const std::vector<HasConstAndNonConstWriteMember> arr(1, value);
    EXPECT_EQ("[T::WriteIntoTracedValue const]", ToString(arr));
  }
}

}  // namespace perfetto
