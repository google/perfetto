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

#include "src/trace_processor/plugins/winscope_importer/deinterning_arg_sink.h"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/common/arg_sink.h"
#include "src/trace_processor/types/variadic.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::winscope {
namespace {

using ::testing::ElementsAre;

struct RecordedArg {
  std::string flat_key;
  std::string key;
  std::string value;

  bool operator==(const RecordedArg& o) const {
    return flat_key == o.flat_key && key == o.key && value == o.value;
  }
};

void PrintTo(const RecordedArg& a, std::ostream* os) {
  *os << "{" << a.flat_key << ", " << a.key << ", " << a.value << "}";
}

class RecordingSink : public ArgSink {
 public:
  explicit RecordingSink(StringPool& pool) : pool_(pool) {}

  void Add(ArgKey key, Variadic value) override {
    std::string v = "other";
    if (value.type == Variadic::kInt) {
      v = "int:" + std::to_string(value.int_value);
    } else if (value.type == Variadic::kUint) {
      v = "uint:" + std::to_string(value.uint_value);
    } else if (value.type == Variadic::kString) {
      v = "str:" + pool_.Get(value.string_value).ToStdString();
    }
    args.push_back({pool_.Get(key.flat_key).ToStdString(),
                    pool_.Get(key.key).ToStdString(), v});
  }

  std::vector<RecordedArg> args;

 private:
  StringPool& pool_;
};

class DeinterningArgSinkTest : public ::testing::Test {
 protected:
  DeinterningArgSinkTest()
      : inner_(pool_),
        sink_(inner_,
              pool_,
              [this](ArgKey key,
                     uint64_t iid) -> std::optional<base::StringView> {
                lookups_.push_back(pool_.Get(key.flat_key).ToStdString());
                if (iid == 42) {
                  return base::StringView("resolved");
                }
                return std::nullopt;
              }) {}

  ArgKey Key(const char* flat_key, const char* key) {
    return ArgKey{pool_.InternString(flat_key), pool_.InternString(key)};
  }
  ArgKey Key(const char* key) { return Key(key, key); }

  StringPool pool_;
  RecordingSink inner_;
  std::vector<std::string> lookups_;
  DeinterningArgSink sink_;
};

TEST_F(DeinterningArgSinkTest, NonIidKeyPassesThrough) {
  sink_.Add(Key("obj.name"), Variadic::Integer(42));
  EXPECT_THAT(inner_.args,
              ElementsAre(RecordedArg{"obj.name", "obj.name", "int:42"}));
  EXPECT_TRUE(lookups_.empty());
}

TEST_F(DeinterningArgSinkTest, NonIntegerIidValuePassesThrough) {
  sink_.Add(Key("obj.name_iid"), Variadic::String(pool_.InternString("hello")));
  EXPECT_THAT(inner_.args, ElementsAre(RecordedArg{
                               "obj.name_iid", "obj.name_iid", "str:hello"}));
  EXPECT_TRUE(lookups_.empty());
}

TEST_F(DeinterningArgSinkTest, IntIidFound) {
  sink_.Add(Key("obj.name_iid"), Variadic::Integer(42));
  EXPECT_THAT(inner_.args,
              ElementsAre(RecordedArg{"obj.name", "obj.name", "str:resolved"}));
  EXPECT_THAT(lookups_, ElementsAre("obj.name_iid"));
}

TEST_F(DeinterningArgSinkTest, UintIidFound) {
  sink_.Add(Key("obj.name_iid"), Variadic::UnsignedInteger(42));
  EXPECT_THAT(inner_.args,
              ElementsAre(RecordedArg{"obj.name", "obj.name", "str:resolved"}));
}

TEST_F(DeinterningArgSinkTest, IntIidNotFound) {
  sink_.Add(Key("obj.name_iid"), Variadic::Integer(7));
  EXPECT_THAT(
      inner_.args,
      ElementsAre(
          RecordedArg{"obj.name", "obj.name", "str:STRING DE-INTERNING ERROR"},
          RecordedArg{"obj.name_iid", "obj.name_iid", "int:7"}));
}

TEST_F(DeinterningArgSinkTest, UintIidNotFound) {
  sink_.Add(Key("obj.name_iid"), Variadic::UnsignedInteger(7));
  EXPECT_THAT(
      inner_.args,
      ElementsAre(
          RecordedArg{"obj.name", "obj.name", "str:STRING DE-INTERNING ERROR"},
          RecordedArg{"obj.name_iid", "obj.name_iid", "uint:7"}));
}

TEST_F(DeinterningArgSinkTest, FlatAndKeyStrippedSeparately) {
  sink_.Add(Key("outer.inner.name_iid", "outer[1].inner.name_iid"),
            Variadic::Integer(42));
  EXPECT_THAT(inner_.args,
              ElementsAre(RecordedArg{"outer.inner.name", "outer[1].inner.name",
                                      "str:resolved"}));
}

// Only |key| is checked for the suffix, so iid array elements are kept as is.
TEST_F(DeinterningArgSinkTest, IidArrayElementPassesThrough) {
  sink_.Add(Key("obj.names_iid", "obj.names_iid[0]"), Variadic::Integer(42));
  EXPECT_THAT(inner_.args, ElementsAre(RecordedArg{
                               "obj.names_iid", "obj.names_iid[0]", "int:42"}));
  EXPECT_TRUE(lookups_.empty());
}

}  // namespace
}  // namespace perfetto::trace_processor::winscope
