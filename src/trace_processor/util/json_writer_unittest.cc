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

#include "src/trace_processor/util/json_writer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::json {
namespace {

// Helper function to strip all whitespace from a string for JSON comparison.
// This allows tests to use nicely formatted expected JSON strings.
std::string StripWhitespace(const std::string& str) {
  std::string result;
  result.reserve(str.size());
  std::copy_if(str.begin(), str.end(), std::back_inserter(result), [](char c) {
    return c != ' ' && c != '\n' && c != '\r' && c != '\t';
  });
  return result;
}

TEST(JsonWriterTest, WriteDictEmpty) {
  std::string result = Write([](JsonValueWriter&& writer) {
    std::move(writer).WriteDict([](JsonDictWriter&) {});
  });
  EXPECT_EQ(result, StripWhitespace("{}"));
}

TEST(JsonWriterTest, WriteDictPrimitives) {
  std::string result = Write([](JsonValueWriter&& writer) {
    std::move(writer).WriteDict([](JsonDictWriter& dict) {
      dict.AddNull("null_value");
      dict.AddBool("bool_true", true);
      dict.AddBool("bool_false", false);
      dict.AddInt("int64", static_cast<int64_t>(-42));
      dict.AddInt("int64_min", static_cast<int64_t>(-9223372036854775807LL));
      dict.AddUint("uint64", static_cast<uint64_t>(42));
      dict.AddUint("uint64_max",
                   static_cast<uint64_t>(18446744073709551615ULL));
      dict.AddDouble("double", 3.14159);
      dict.AddString("string", "hello");
      dict.AddString("string_view", std::string_view("world"));
      dict.AddString("quotes", "say\"hello\"");
      dict.AddString("backslash", "path\\to\\file");
      dict.AddString("control", "\x01\x02\x1f");
      // Escaped keys
      dict.AddString("key\"with\"quotes", "value1");
      dict.AddString("key\\with\\backslash", "value2");
      dict.AddString("key\nwith\nnewline", "value3");
      dict.AddString("key\twith\ttab", "value4");
      dict.AddString("key\x01with_control", "value5");
    });
  });

  std::string expected = R"({
    "null_value": null,
    "bool_true": true,
    "bool_false": false,
    "int64": -42,
    "int64_min": -9223372036854775807,
    "uint64": 42,
    "uint64_max": 18446744073709551615,
    "double": 3.141590,
    "string": "hello",
    "string_view": "world",
    "quotes": "say\"hello\"",
    "backslash": "path\\to\\file",
    "control": "\u0001\u0002\u001f",
    "key\"with\"quotes": "value1",
    "key\\with\\backslash": "value2",
    "key\nwith\nnewline": "value3",
    "key\twith\ttab": "value4",
    "key\u0001with_control": "value5"
  })";

  EXPECT_EQ(result, StripWhitespace(expected));
}

TEST(JsonWriterTest, WriteDictWhitespace) {
  std::string result = Write([](JsonValueWriter&& writer) {
    std::move(writer).WriteDict([](JsonDictWriter& dict) {
      dict.AddString("newline", "line1\nline2");
      dict.AddString("carriage", "line1\rline2");
      dict.AddString("tab", "col1\tcol2");
    });
  });

  std::string expected =
      R"({"newline":"line1\nline2","carriage":"line1\rline2","tab":"col1\tcol2"})";

  EXPECT_EQ(result, expected);
}

TEST(JsonWriterTest, WriteDictSpecialDoubles) {
  std::string result = Write([](JsonValueWriter&& writer) {
    std::move(writer).WriteDict([](JsonDictWriter& dict) {
      dict.AddDouble("nan", std::nan(""));
      dict.AddDouble("inf", std::numeric_limits<double>::infinity());
      dict.AddDouble("neg_inf", -std::numeric_limits<double>::infinity());
    });
  });

  std::string expected = R"({
    "nan": "NaN",
    "inf": "Infinity",
    "neg_inf": "-Infinity"
  })";

  EXPECT_EQ(result, StripWhitespace(expected));
}

TEST(JsonWriterTest, WriteDictNested) {
  std::string result = Write([](JsonValueWriter&& writer) {
    std::move(writer).WriteDict([](JsonDictWriter& dict) {
      dict.AddString("outer", "value");
      dict.AddDict("nested", [](JsonDictWriter& nested) {
        nested.AddInt("inner1", static_cast<int64_t>(42));
        nested.AddString("inner2", "text");
      });
      dict.AddArray("items", [](JsonArrayWriter& arr) {
        arr.AppendInt(static_cast<int64_t>(1));
        arr.AppendInt(static_cast<int64_t>(2));
        arr.AppendInt(static_cast<int64_t>(3));
      });
    });
  });

  std::string expected = R"({
    "outer": "value",
    "nested": {
      "inner1": 42,
      "inner2": "text"
    },
    "items": [1, 2, 3]
  })";

  EXPECT_EQ(result, StripWhitespace(expected));
}

TEST(JsonWriterTest, WriteArrayEmpty) {
  std::string result = Write([](JsonValueWriter&& writer) {
    std::move(writer).WriteArray([](JsonArrayWriter&) {});
  });
  EXPECT_EQ(result, StripWhitespace("[]"));
}

TEST(JsonWriterTest, WriteArrayPrimitives) {
  std::string result = Write([](JsonValueWriter&& writer) {
    std::move(writer).WriteArray([](JsonArrayWriter& arr) {
      arr.AppendNull();
      arr.AppendBool(true);
      arr.AppendBool(false);
      arr.AppendInt(static_cast<int64_t>(-42));
      arr.AppendInt(static_cast<int64_t>(-9223372036854775807LL));
      arr.AppendUint(static_cast<uint64_t>(42));
      arr.AppendUint(static_cast<uint64_t>(18446744073709551615ULL));
      arr.AppendDouble(3.14159);
      arr.AppendString("hello");
      arr.AppendString(std::string_view("world"));
    });
  });

  std::string expected = R"([
    null,
    true,
    false,
    -42,
    -9223372036854775807,
    42,
    18446744073709551615,
    3.141590,
    "hello",
    "world"
  ])";

  EXPECT_EQ(result, StripWhitespace(expected));
}

TEST(JsonWriterTest, WriteArrayNested) {
  std::string result = Write([](JsonValueWriter&& writer) {
    std::move(writer).WriteArray([](JsonArrayWriter& arr) {
      arr.AppendString("first");
      arr.AppendDict([](JsonDictWriter& dict) {
        dict.AddInt("key1", static_cast<int64_t>(42));
        dict.AddString("key2", "value");
      });
      arr.AppendInt(static_cast<int64_t>(1));
      arr.AppendArray([](JsonArrayWriter& nested) {
        nested.AppendInt(static_cast<int64_t>(2));
        nested.AppendInt(static_cast<int64_t>(3));
      });
      arr.AppendInt(static_cast<int64_t>(4));
      arr.AppendString("last");
    });
  });

  std::string expected = R"([
    "first",
    {
      "key1": 42,
      "key2": "value"
    },
    1,
    [2, 3],
    4,
    "last"
  ])";

  EXPECT_EQ(result, StripWhitespace(expected));
}

TEST(JsonWriterTest, Complex) {
  std::string result = Write([](JsonValueWriter&& writer) {
    std::move(writer).WriteDict([](JsonDictWriter& dict) {
      dict.AddString("name", "root");
      dict.AddArray("items", [](JsonArrayWriter& arr) {
        arr.AppendDict([](JsonDictWriter& obj1) {
          obj1.AddInt("id", static_cast<int64_t>(1));
          obj1.AddArray("tags", [](JsonArrayWriter& tags) {
            tags.AppendString("tag1");
            tags.AppendString("tag2");
          });
        });
        arr.AppendDict([](JsonDictWriter& obj2) {
          obj2.AddInt("id", static_cast<int64_t>(2));
          obj2.AddNull("value");
        });
      });
      dict.AddDict("metadata", [](JsonDictWriter& meta) {
        meta.AddInt("version", static_cast<int64_t>(1));
        meta.AddBool("created", true);
      });
    });
  });

  std::string expected = R"({
    "name": "root",
    "items": [
      {
        "id": 1,
        "tags": ["tag1", "tag2"]
      },
      {
        "id": 2,
        "value": null
      }
    ],
    "metadata": {
      "version": 1,
      "created": true
    }
  })";

  EXPECT_EQ(result, StripWhitespace(expected));
}

}  // namespace
}  // namespace perfetto::trace_processor::json
