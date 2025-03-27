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

#include "src/trace_processor/dataframe/impl/bytecode_interpreter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest-param-test.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/dataframe/impl/bytecode_core.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe::impl::bytecode {
namespace {
using testing::AllOf;
using testing::Ge;
using testing::Le;

std::string FilterValueToString(const FilterSpec::Value& value) {
  switch (value.index()) {
    case base::variant_index<FilterSpec::Value, nullptr_t>():
      return "nullptr";
    case base::variant_index<FilterSpec::Value, int64_t>(): {
      auto res = base::unchecked_get<int64_t>(value);
      return res < 0 ? "neg_" + std::to_string(-res) : std::to_string(res);
    }
    case base::variant_index<FilterSpec::Value, double>(): {
      std::string no_dot = base::ReplaceAll(
          std::to_string(base::unchecked_get<double>(value)), ".", "_");
      return base::ReplaceAll(no_dot, "-", "neg_");
    }
    case base::variant_index<FilterSpec::Value, const char*>():
      return {base::unchecked_get<const char*>(value)};
    default:
      PERFETTO_FATAL("Unknown filter value type");
  }
}

std::string CastResultToString(const CastFilterValueResult& res) {
  if (res.validity == CastFilterValueResult::Validity::kValid) {
    switch (res.value.index()) {
      case base::variant_index<CastFilterValueResult::Value,
                               CastFilterValueResult::Id>(): {
        const auto& id =
            base::unchecked_get<CastFilterValueResult::Id>(res.value);
        return "Id_" + std::to_string(id.value);
      }
      default:
        PERFETTO_FATAL("Unknown filter value type");
    }
  }
  return res.validity == CastFilterValueResult::Validity::kNoneMatch
             ? "NoneMatch"
             : "AllMatch";
}

Bytecode ParseBytecode(const std::string& bytecode_str) {
#define PERFETTO_DATAFRAME_BYTECODE_AS_STRING(...) #__VA_ARGS__,

  std::vector<const char*> bytecode_names{
      PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_AS_STRING)};

#define PERFETTO_DATAFRAME_BYTECODE_OFFSETS(...) __VA_ARGS__::kOffsets,
  std::vector<std::array<uint32_t, 6>> offsets{
      PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_OFFSETS)};

#define PERFETTO_DATAFRAME_BYTECODE_NAMES(...) __VA_ARGS__::kNames,
  std::vector<std::array<const char*, 5>> names{
      PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_NAMES)};

  Bytecode bc;
  size_t colon_pos = bytecode_str.find(": ");
  PERFETTO_CHECK(colon_pos != std::string::npos);
  {
    auto it = std::find(bytecode_names.begin(), bytecode_names.end(),
                        bytecode_str.substr(0, colon_pos));
    PERFETTO_CHECK(it != bytecode_names.end());
    bc.option = it - bytecode_names.begin();
  }

  // Trim away the [ and ] from the bytecode string.
  std::string args_str = bytecode_str.substr(colon_pos + 2);
  PERFETTO_CHECK(args_str.front() == '[');
  PERFETTO_CHECK(args_str.back() == ']');
  args_str = args_str.substr(1, args_str.size() - 2);

  const auto& cur_offset = offsets[bc.option];
  std::vector<std::string> args = base::SplitString(args_str, ", ");
  for (const auto& arg : args) {
    auto res = base::SplitString(arg, "=");
    PERFETTO_CHECK(res.size() == 2);

    // Remove everything before the first "(" (which may not be the first
    // character) and after the last ")".
    std::string arg_val;
    if (size_t open = res[1].find('('); open != std::string_view::npos) {
      arg_val = res[1].substr(open + 1, res[1].rfind(')') - open - 1);
    } else {
      arg_val = res[1];
    }

    auto* it =
        std::find(names[bc.option].begin(), names[bc.option].end(), res[0]);
    PERFETTO_CHECK(it != names[bc.option].end());
    uint32_t arg_idx = it - names[bc.option].begin();

    uint32_t size = cur_offset[arg_idx + 1] - cur_offset[arg_idx];
    if (size == 4) {
      auto val = base::StringToInt32(arg_val);
      PERFETTO_CHECK(val.has_value());
      memcpy(&bc.args_buffer[cur_offset[arg_idx]], &val, 4);
    } else if (size == 8) {
      auto val = base::StringToInt64(arg_val);
      PERFETTO_CHECK(val.has_value());
      memcpy(&bc.args_buffer[cur_offset[arg_idx]], &val, 8);
    } else {
      PERFETTO_CHECK(false);
    }
  }
  return bc;
}

TEST(BytecodeInterpreterTest, InitRange) {
  BytecodeVector bytecode;
  bytecode.emplace_back(
      ParseBytecode("InitRange: [size=134, dest_register=Register(0)]"));

  Interpreter interpreter(std::move(bytecode), {}, nullptr, nullptr);
  interpreter.Execute();
  const auto* result = interpreter.GetRegisterValue(reg::ReadHandle<Range>(0));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->b, 0u);
  EXPECT_EQ(result->e, 134u);
}

TEST(BytecodeInterpreterTest, AllocateIndices) {
  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "AllocateIndices: [size=132, dest_slab_register=Register(0), "
      "dest_span_register=Register(1)]"));

  Interpreter interpreter(std::move(bytecode), {}, nullptr, nullptr);
  interpreter.Execute();

  const auto* slab =
      interpreter.GetRegisterValue(reg::ReadHandle<Slab<uint32_t>>(0));
  {
    ASSERT_TRUE(slab);
    EXPECT_EQ(slab->size(), 132u);
  }
  {
    const auto* span =
        interpreter.GetRegisterValue(reg::ReadHandle<Span<uint32_t>>(1));
    ASSERT_TRUE(span);
    EXPECT_EQ(span->size(), 132u);
    EXPECT_EQ(span->b, slab->begin());
    EXPECT_EQ(span->e, slab->end());
  }
}

TEST(BytecodeInterpreterTest, AllocateIndicesAlreadyAllocated) {
  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "AllocateIndices: [size=132, dest_slab_register=Register(0), "
      "dest_span_register=Register(1)]"));

  Interpreter interpreter(std::move(bytecode), {}, nullptr, nullptr);
  auto existing_slab = Slab<uint32_t>::Alloc(132u);
  auto* expected_begin = existing_slab.begin();
  auto* expected_end = existing_slab.end();
  interpreter.SetRegisterValueForTesting(reg::WriteHandle<Slab<uint32_t>>(0),
                                         std::move(existing_slab));
  interpreter.Execute();

  const auto* slab =
      interpreter.GetRegisterValue(reg::ReadHandle<Slab<uint32_t>>(0));
  {
    EXPECT_EQ(slab->begin(), expected_begin);
    EXPECT_EQ(slab->end(), expected_end);
  }
  {
    const auto* span =
        interpreter.GetRegisterValue(reg::ReadHandle<Span<uint32_t>>(1));
    ASSERT_TRUE(span);
    EXPECT_EQ(span->size(), 132u);
    EXPECT_EQ(span->b, slab->begin());
    EXPECT_EQ(span->e, slab->end());
  }
}

TEST(BytecodeInterpreterTest, Iota) {
  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "Iota: [source_register=Register(0), update_register=Register(1)]"));

  auto slab = Slab<uint32_t>::Alloc(132u);
  Interpreter interpreter(std::move(bytecode), {}, nullptr, nullptr);
  interpreter.SetRegisterValueForTesting(reg::WriteHandle<Range>(0),
                                         Range{5, 10});
  interpreter.SetRegisterValueForTesting(
      reg::WriteHandle<Span<uint32_t>>(1),
      Span<uint32_t>{slab.begin(), slab.end()});
  interpreter.Execute();

  const auto* update =
      interpreter.GetRegisterValue(reg::ReadHandle<Span<uint32_t>>(1));
  ASSERT_TRUE(update);
  ASSERT_THAT(update->b, AllOf(Ge(slab.begin()), Le(slab.end())));
  ASSERT_THAT(update->e, AllOf(Ge(slab.begin()), Le(slab.end())));
  EXPECT_THAT(std::vector<uint32_t>(update->b, update->e),
              testing::ElementsAreArray({5, 6, 7, 8, 9}));
}

using FilterValue = FilterSpec::Value;
using CastResult = CastFilterValueResult;

struct CastTestCase {
  FilterValue input;
  CastResult expected;
  Op op = Eq{};
};

class BytecodeInterpreterCastTest
    : public testing::TestWithParam<CastTestCase> {};

TEST_P(BytecodeInterpreterCastTest, Cast) {
  const auto& [input, expected, op] = GetParam();

  BytecodeVector bytecode;
  bytecode.emplace_back(
      ParseBytecode(base::StackString<1024>(
                        "CastFilterValue<Id>: [fval_handle=FilterValue(0), "
                        "write_register=Register(0), op=Op(%u)]",
                        op.index())
                        .ToStdString()));

  Interpreter interpreter(std::move(bytecode), &input, nullptr, nullptr);
  interpreter.Execute();

  const auto* result =
      interpreter.GetRegisterValue(reg::ReadHandle<CastFilterValueResult>(0));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->validity, expected.validity);
  if (result->validity == CastResult::Validity::kValid) {
    EXPECT_EQ(result->value, expected.value);
  }
}

INSTANTIATE_TEST_SUITE_P(
    IntegerToInteger,
    BytecodeInterpreterCastTest,
    testing::Values(
        CastTestCase{
            FilterValue{1024},
            CastResult::Valid(CastResult::Id{1024}),
        },
        CastTestCase{
            FilterValue{int64_t(std::numeric_limits<uint32_t>::max()) + 1},
            CastResult::NoneMatch(),
        },
        CastTestCase{
            FilterValue{int64_t(std::numeric_limits<uint32_t>::min()) - 1},
            CastResult::NoneMatch(),
        }),
    [](const testing::TestParamInfo<BytecodeInterpreterCastTest::ParamType>&
           info) {
      return FilterValueToString(info.param.input) + "_" +
             CastResultToString(info.param.expected);
    });

INSTANTIATE_TEST_SUITE_P(
    DoubleToInteger,
    BytecodeInterpreterCastTest,
    testing::Values(
        CastTestCase{
            FilterValue{1024.0},
            CastResult::Valid(CastResult::Id{1024}),
        },
        CastTestCase{FilterValue{1024.1}, CastResult::NoneMatch()},
        CastTestCase{FilterValue{1024.9}, CastResult::NoneMatch()},
        CastTestCase{FilterValue{NAN}, CastResult::NoneMatch()},
        CastTestCase{
            FilterValue{double(std::numeric_limits<uint32_t>::max()) + 1},
            CastResult::NoneMatch(),
        },
        CastTestCase{
            FilterValue{double(std::numeric_limits<uint32_t>::min()) - 1},
            CastResult::NoneMatch(),
        }),
    [](const testing::TestParamInfo<BytecodeInterpreterCastTest::ParamType>&
           info) {
      return FilterValueToString(info.param.input) + "_" +
             CastResultToString(info.param.expected);
    });

TEST(BytecodeInterpreterTest, SortedFilterId) {
  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "SortedFilter<Id, EqualRange>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(0)]"));

  Interpreter interpreter(std::move(bytecode), nullptr, nullptr, nullptr);

  // Test case 1: Value exists in range
  interpreter.SetRegisterValueForTesting(
      reg::WriteHandle<CastFilterValueResult>(0),
      CastFilterValueResult::Valid(CastFilterValueResult::Id{5}));
  interpreter.SetRegisterValueForTesting(reg::WriteHandle<Range>(1),
                                         Range{0, 10});
  interpreter.Execute();

  const auto* result = interpreter.GetRegisterValue(reg::ReadHandle<Range>(1));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->b, 5u);
  EXPECT_EQ(result->e, 6u);

  // Test case 2: Value doesn't exist in range
  interpreter.SetRegisterValueForTesting(
      reg::WriteHandle<CastFilterValueResult>(0),
      CastFilterValueResult::Valid(CastFilterValueResult::Id{15}));
  interpreter.SetRegisterValueForTesting(reg::WriteHandle<Range>(1),
                                         Range{0, 10});
  interpreter.Execute();

  result = interpreter.GetRegisterValue(reg::ReadHandle<Range>(1));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->size(), 0u);

  // Test case 3: Invalid cast result (NoneMatch)
  interpreter.SetRegisterValueForTesting(
      reg::WriteHandle<CastFilterValueResult>(0),
      CastFilterValueResult::NoneMatch());
  interpreter.SetRegisterValueForTesting(reg::WriteHandle<Range>(1),
                                         Range{0, 10});
  interpreter.Execute();

  result = interpreter.GetRegisterValue(reg::ReadHandle<Range>(1));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->size(), 0u);
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe::impl::bytecode
