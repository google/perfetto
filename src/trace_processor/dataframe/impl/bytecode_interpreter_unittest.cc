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
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/dataframe/impl/bytecode_core.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/flex_vector.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/value_fetcher.h"
#include "test/gtest_and_gmock.h"

namespace {

using testing::AllOf;
using testing::ElementsAre;
using testing::IsEmpty;

}  // namespace

namespace perfetto::trace_processor::dataframe::impl::bytecode {
namespace {

using FilterValue = std::variant<int64_t, double, const char*, nullptr_t>;

struct Fetcher : ValueFetcher {
  using Type = size_t;
  static constexpr Type kInt64 = base::variant_index<FilterValue, int64_t>();
  static constexpr Type kDouble = base::variant_index<FilterValue, double>();
  static constexpr Type kString =
      base::variant_index<FilterValue, const char*>();
  static constexpr Type kNull = base::variant_index<FilterValue, nullptr_t>();

  // Fetches an int64_t value at the given index.
  int64_t Int64Value(uint32_t idx) const {
    return std::get<int64_t>(values[idx]);
  }
  // Fetches a double value at the given index.
  double DoubleValue(uint32_t idx) const {
    return std::get<double>(values[idx]);
  }
  // Fetches a string value at the given index.
  const char* StringValue(uint32_t idx) const {
    return std::get<const char*>(values[idx]);
  }
  // Fetches the type of the value at the given index.
  Type ValueType(uint32_t idx) const { return values[idx].index(); }

  const FilterValue* values;
};

std::string FixNegativeAndDecimal(const std::string& str) {
  return base::ReplaceAll(base::ReplaceAll(str, ".", "_"), "-", "neg_");
}

template <typename... Ts>
void SetRegistersAndExecute(Interpreter<Fetcher>& interpreter,
                            Fetcher& fetcher,
                            Ts... value) {
  size_t i = 0;
  (interpreter.SetRegisterValueForTesting(reg::WriteHandle<Ts>(i++),
                                          std::move(value)),
   ...);
  interpreter.Execute(fetcher);
}

template <typename T>
Span<T> GetSpan(std::vector<T>& vec) {
  return Span<T>{vec.data(), vec.data() + vec.size()};
}

template <typename T>
const T& GetRegister(Interpreter<Fetcher>& interpreter, uint32_t reg_idx) {
  const auto* r = interpreter.GetRegisterValue(reg::ReadHandle<T>(reg_idx));
  PERFETTO_CHECK(r);
  return *r;
}

std::string FilterValueToString(const FilterValue& value) {
  switch (value.index()) {
    case base::variant_index<FilterValue, nullptr_t>():
      return "nullptr";
    case base::variant_index<FilterValue, int64_t>(): {
      auto res = base::unchecked_get<int64_t>(value);
      return FixNegativeAndDecimal(std::to_string(res));
    }
    case base::variant_index<FilterValue, double>(): {
      auto res = base::unchecked_get<double>(value);
      return FixNegativeAndDecimal(std::to_string(res));
    }
    case base::variant_index<FilterValue, const char*>():
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
        return "Id_" + FixNegativeAndDecimal(std::to_string(id.value));
      }
      case base::variant_index<CastFilterValueResult::Value, uint32_t>(): {
        const auto& uint32 = base::unchecked_get<uint32_t>(res.value);
        return "Uint32_" + FixNegativeAndDecimal(std::to_string(uint32));
      }
      case base::variant_index<CastFilterValueResult::Value, int32_t>(): {
        const auto& int32 = base::unchecked_get<int32_t>(res.value);
        return "Int32_" + FixNegativeAndDecimal(std::to_string(int32));
      }
      case base::variant_index<CastFilterValueResult::Value, int64_t>(): {
        const auto& int64 = base::unchecked_get<int64_t>(res.value);
        return "Int64_" + FixNegativeAndDecimal(std::to_string(int64));
      }
      case base::variant_index<CastFilterValueResult::Value, double>(): {
        const auto& d = base::unchecked_get<double>(res.value);
        return "Double_" + FixNegativeAndDecimal(std::to_string(d));
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
  static constexpr uint32_t kNumBytecodeCount =
      std::variant_size_v<BytecodeVariant>;

#define PERFETTO_DATAFRAME_BYTECODE_AS_STRING(...) #__VA_ARGS__,
  static constexpr std::array<const char*, kNumBytecodeCount> bytecode_names{
      PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_AS_STRING)};

#define PERFETTO_DATAFRAME_BYTECODE_OFFSETS(...) __VA_ARGS__::kOffsets,
  static constexpr std::array<std::array<uint32_t, 6>, kNumBytecodeCount>
      offsets{PERFETTO_DATAFRAME_BYTECODE_LIST(
          PERFETTO_DATAFRAME_BYTECODE_OFFSETS)};

#define PERFETTO_DATAFRAME_BYTECODE_NAMES(...) __VA_ARGS__::kNames,
  static constexpr std::array<std::array<const char*, 5>, kNumBytecodeCount>
      names{
          PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_NAMES)};

  Bytecode bc;
  size_t colon_pos = bytecode_str.find(": ");
  PERFETTO_CHECK(colon_pos != std::string::npos);
  {
    const auto* it = std::find(bytecode_names.data(),
                               bytecode_names.data() + bytecode_names.size(),
                               bytecode_str.substr(0, colon_pos));
    PERFETTO_CHECK(it != bytecode_names.data() + bytecode_names.size());
    bc.option = static_cast<uint32_t>(it - bytecode_names.data());
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

    const auto* it =
        std::find(names[bc.option].data(),
                  names[bc.option].data() + names[bc.option].size(), res[0]);
    PERFETTO_CHECK(it != names[bc.option].data() + names[bc.option].size());
    uint32_t arg_idx = static_cast<uint32_t>(it - names[bc.option].data());

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

template <typename T>
FlexVector<T> CreateFlexVectorForTesting(std::initializer_list<T> values) {
  FlexVector<T> vec;
  for (const auto& value : values) {
    vec.push_back(value);
  }
  return vec;
}

TEST(BytecodeInterpreterTest, InitRange) {
  BytecodeVector bytecode;
  bytecode.emplace_back(
      ParseBytecode("InitRange: [size=134, dest_register=Register(0)]"));

  Interpreter<Fetcher> interpreter(std::move(bytecode), nullptr, nullptr);
  Fetcher fetcher{{}, nullptr};
  SetRegistersAndExecute(interpreter, fetcher);

  const auto& result = GetRegister<Range>(interpreter, 0);
  EXPECT_EQ(result.b, 0u);
  EXPECT_EQ(result.e, 134u);
}

TEST(BytecodeInterpreterTest, AllocateIndices) {
  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "AllocateIndices: [size=132, dest_slab_register=Register(0), "
      "dest_span_register=Register(1)]"));

  Interpreter<Fetcher> interpreter(std::move(bytecode), nullptr, nullptr);
  Fetcher fetcher{{}, nullptr};
  SetRegistersAndExecute(interpreter, fetcher);

  const auto& slab = GetRegister<Slab<uint32_t>>(interpreter, 0);
  {
    EXPECT_EQ(slab.size(), 132u);
  }
  {
    const auto& span = GetRegister<Span<uint32_t>>(interpreter, 1);
    EXPECT_EQ(span.size(), 132u);
    EXPECT_EQ(span.b, slab.begin());
    EXPECT_EQ(span.e, slab.end());
  }
}

TEST(BytecodeInterpreterTest, AllocateIndicesAlreadyAllocated) {
  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "AllocateIndices: [size=132, dest_slab_register=Register(0), "
      "dest_span_register=Register(1)]"));

  Interpreter<Fetcher> interpreter(std::move(bytecode), nullptr, nullptr);
  auto existing_slab = Slab<uint32_t>::Alloc(132u);
  auto* expected_begin = existing_slab.begin();
  auto* expected_end = existing_slab.end();
  Fetcher fetcher{{}, nullptr};
  SetRegistersAndExecute(interpreter, fetcher, std::move(existing_slab));

  const auto& slab = GetRegister<Slab<uint32_t>>(interpreter, 0);
  {
    EXPECT_EQ(slab.begin(), expected_begin);
    EXPECT_EQ(slab.end(), expected_end);
  }
  {
    const auto& span = GetRegister<Span<uint32_t>>(interpreter, 1);
    EXPECT_EQ(span.size(), 132u);
    EXPECT_EQ(span.b, slab.begin());
    EXPECT_EQ(span.e, slab.end());
  }
}

TEST(BytecodeInterpreterTest, Iota) {
  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "Iota: [source_register=Register(0), update_register=Register(1)]"));

  std::vector<uint32_t> res(132u);
  Fetcher fetcher{{}, nullptr};
  Interpreter<Fetcher> interpreter(std::move(bytecode), nullptr, nullptr);
  SetRegistersAndExecute(interpreter, fetcher, Range{5, 10}, GetSpan(res));

  const auto& update = GetRegister<Span<uint32_t>>(interpreter, 1);
  ASSERT_THAT(update.b, AllOf(testing::Ge(res.data()),
                              testing::Le(res.data() + res.size())));
  ASSERT_THAT(update.e, AllOf(testing::Ge(res.data()),
                              testing::Le(res.data() + res.size())));
  EXPECT_THAT(update, testing::ElementsAreArray({5, 6, 7, 8, 9}));
}

using CastResult = CastFilterValueResult;

struct CastTestCase {
  std::string input_type;
  FilterValue input;
  CastResult expected;
  Op op = dataframe::Eq{};
};

class BytecodeInterpreterCastTest
    : public testing::TestWithParam<CastTestCase> {};

TEST_P(BytecodeInterpreterCastTest, Cast) {
  const auto& [input_type, input, expected, op] = GetParam();

  BytecodeVector bytecode;
  bytecode.emplace_back(
      ParseBytecode(base::StackString<1024>(
                        "CastFilterValue<%s>: [fval_handle=FilterValue(0), "
                        "write_register=Register(0), op=Op(%u)]",
                        input_type.c_str(), op.index())
                        .ToStdString()));

  Interpreter<Fetcher> interpreter(std::move(bytecode), nullptr, nullptr);
  Fetcher fetcher{{}, &input};
  SetRegistersAndExecute(interpreter, fetcher);

  const auto& result = GetRegister<CastFilterValueResult>(interpreter, 0);
  ASSERT_THAT(result.validity, testing::Eq(expected.validity));
  if (result.validity == CastResult::Validity::kValid) {
    ASSERT_THAT(result.value, testing::Eq(expected.value));
  }
}

INSTANTIATE_TEST_SUITE_P(
    IntegerToInteger,
    BytecodeInterpreterCastTest,
    testing::Values(
        CastTestCase{
            "Id",
            FilterValue{1024l},
            CastResult::Valid(CastResult::Id{1024}),
        },
        CastTestCase{
            "Id",
            FilterValue{int64_t(std::numeric_limits<uint32_t>::max()) + 1},
            CastResult::NoneMatch(),
        },
        CastTestCase{
            "Id",
            FilterValue{int64_t(std::numeric_limits<uint32_t>::min()) - 1},
            CastResult::NoneMatch(),
        },
        CastTestCase{
            "Uint32",
            FilterValue{1024l},
            CastResult::Valid(uint32_t(1024)),
        },
        CastTestCase{
            "Int64",
            FilterValue{std::numeric_limits<int64_t>::max()},
            CastResult::Valid(std::numeric_limits<int64_t>::max()),
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
            "Id",
            FilterValue{1024.0},
            CastResult::Valid(CastResult::Id{1024}),
        },
        CastTestCase{"Id", FilterValue{1024.1}, CastResult::NoneMatch()},
        CastTestCase{"Id", FilterValue{1024.9}, CastResult::NoneMatch()},
        CastTestCase{"Id", FilterValue{NAN}, CastResult::NoneMatch()},
        CastTestCase{
            "Id",
            FilterValue{double(std::numeric_limits<uint32_t>::max()) + 1},
            CastResult::NoneMatch(),
        },
        CastTestCase{
            "Id",
            FilterValue{double(std::numeric_limits<uint32_t>::min()) - 1},
            CastResult::NoneMatch(),
        },
        CastTestCase{
            "Uint32",
            FilterValue{1024.0},
            CastResult::Valid(uint32_t(1024)),
        },
        CastTestCase{
            "Int64",
            FilterValue{-9223372036854775808.0},
            CastResult::Valid(int64_t(-9223372036854775807ll - 1)),
        },
        CastTestCase{
            "Int64",
            FilterValue{9223372036854775808.0},
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

  Interpreter<Fetcher> interpreter(std::move(bytecode), nullptr, nullptr);
  Fetcher fetcher{{}, nullptr};
  {
    // Test case 1: Value exists in range
    SetRegistersAndExecute(
        interpreter, fetcher,
        CastFilterValueResult::Valid(CastFilterValueResult::Id{5}),
        Range{0, 10});

    const auto& result = GetRegister<Range>(interpreter, 1);
    EXPECT_EQ(result.b, 5u);
    EXPECT_EQ(result.e, 6u);
  }
  {
    // Test case 2: Value below range
    SetRegistersAndExecute(
        interpreter, fetcher,
        CastFilterValueResult::Valid(CastFilterValueResult::Id{2}),
        Range{3, 10});

    const auto& result = GetRegister<Range>(interpreter, 1);
    EXPECT_EQ(result.size(), 0u);
  }
  {
    // Test case 3: Invalid cast result (NoneMatch)
    SetRegistersAndExecute(interpreter, fetcher,
                           CastFilterValueResult::NoneMatch(), Range{0, 10});

    const auto& result = GetRegister<Range>(interpreter, 1);
    EXPECT_EQ(result.size(), 0u);
  }
}

TEST(BytecodeInterpreterTest, SortedFilterUint32) {
  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "SortedFilter<Uint32, EqualRange>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(0)]"));

  auto values =
      CreateFlexVectorForTesting<uint32_t>({0, 4u, 5u, 5u, 5u, 6u, 10u, 10u});
  Column column{ColumnSpec{"foo", Uint32(), Sorted(), NonNull()},
                Storage{std::move(values)}, Overlay{Overlay::NoOverlay{}}};
  Interpreter<Fetcher> interpreter(std::move(bytecode), &column, nullptr);
  Fetcher fetcher{{}, nullptr};
  {
    // Test case 1: Value exists in range
    SetRegistersAndExecute(interpreter, fetcher,
                           CastFilterValueResult::Valid(5u), Range{3u, 8u});
    const auto& result = GetRegister<Range>(interpreter, 1);
    EXPECT_EQ(result.b, 3u);
    EXPECT_EQ(result.e, 5u);
  }
  {
    // Test case 2: Value exists not range
    SetRegistersAndExecute(interpreter, fetcher,
                           CastFilterValueResult::Valid(4u), Range{3u, 8u});
    const auto& result = GetRegister<Range>(interpreter, 1);
    EXPECT_EQ(result.size(), 0u);
  }
  {
    // Test case 3: Invalid cast result (NoneMatch)
    SetRegistersAndExecute(interpreter, fetcher,
                           CastFilterValueResult::NoneMatch(), Range{0, 8u});
    const auto& result = GetRegister<Range>(interpreter, 1);
    EXPECT_EQ(result.size(), 0u);
  }
}

TEST(BytecodeInterpreterTest, FilterId) {
  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "NonStringFilter<Id, Eq>: [col=0, val_register=Register(0), "
      "source_register=Register(1), update_register=Register(2)]"));

  Fetcher fetcher{{}, nullptr};
  Interpreter<Fetcher> interpreter(std::move(bytecode), nullptr, nullptr);
  std::vector<uint32_t> indices_spec = {12, 44, 10, 4, 5, 2, 3};
  {
    // Test case 1: Value exists in range
    std::vector<uint32_t> indices = indices_spec;
    SetRegistersAndExecute(
        interpreter, fetcher,
        CastFilterValueResult::Valid(CastFilterValueResult::Id{5}),
        GetSpan(indices), GetSpan(indices));

    const auto& result = GetRegister<Span<uint32_t>>(interpreter, 2);
    EXPECT_THAT(result, ElementsAre(5));
  }
  {
    // Test case 2: Value above range
    std::vector<uint32_t> indices = indices_spec;
    SetRegistersAndExecute(
        interpreter, fetcher,
        CastFilterValueResult::Valid(CastFilterValueResult::Id{11}),
        GetSpan(indices), GetSpan(indices));

    const auto& result = GetRegister<Span<uint32_t>>(interpreter, 2);
    EXPECT_THAT(result, IsEmpty());
  }
  {
    // Test case 3: Invalid cast result (NoneMatch)
    std::vector<uint32_t> indices = indices_spec;
    SetRegistersAndExecute(interpreter, fetcher,
                           CastFilterValueResult::NoneMatch(), GetSpan(indices),
                           GetSpan(indices));

    const auto& result = GetRegister<Span<uint32_t>>(interpreter, 2);
    EXPECT_THAT(result, IsEmpty());
  }
}

TEST(BytecodeInterpreterTest, StrideCopy) {
  BytecodeVector bytecode;
  bytecode.emplace_back(
      ParseBytecode("StrideCopy: [source_register=Register(0), "
                    "update_register=Register(1), stride=3]"));

  Fetcher fetcher{{}, nullptr};
  Interpreter<Fetcher> interpreter(std::move(bytecode), nullptr, nullptr);
  std::vector<uint32_t> source = {10, 3, 12, 4};
  std::vector<uint32_t> dest(source.size() * 3);
  SetRegistersAndExecute(interpreter, fetcher, GetSpan(source), GetSpan(dest));

  const auto& result = GetRegister<Span<uint32_t>>(interpreter, 1);
  EXPECT_THAT(result, ElementsAre(10, 0, 0, 3, 0, 0, 12, 0, 0, 4, 0, 0));
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe::impl::bytecode
