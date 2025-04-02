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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/impl/bytecode_core.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/flex_vector.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/value_fetcher.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe::impl::bytecode {
namespace {

using testing::AllOf;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::IsEmpty;
using testing::SizeIs;

using FilterValue = std::variant<int64_t, double, const char*, nullptr_t>;

struct Fetcher : ValueFetcher {
  using Type = size_t;
  static constexpr Type kInt64 = base::variant_index<FilterValue, int64_t>();
  static constexpr Type kDouble = base::variant_index<FilterValue, double>();
  static constexpr Type kString =
      base::variant_index<FilterValue, const char*>();
  static constexpr Type kNull = base::variant_index<FilterValue, nullptr_t>();

  // Fetches an int64_t value at the given index.
  int64_t GetInt64Value(uint32_t idx) const {
    PERFETTO_CHECK(idx == 0);
    return std::get<int64_t>(value);
  }
  // Fetches a double value at the given index.
  double GetDoubleValue(uint32_t idx) const {
    PERFETTO_CHECK(idx == 0);
    return std::get<double>(value);
  }
  // Fetches a string value at the given index.
  const char* GetStringValue(uint32_t idx) const {
    PERFETTO_CHECK(idx == 0);
    return std::get<const char*>(value);
  }
  // Fetches the type of the value at the given index.
  Type GetValueType(uint32_t idx) const {
    PERFETTO_CHECK(idx == 0);
    return value.index();
  }

  FilterValue value;
};

std::string FixNegativeAndDecimal(const std::string& str) {
  return base::ReplaceAll(base::ReplaceAll(str, ".", "_"), "-", "neg_");
}

std::string ValToString(const FilterValue& value) {
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

std::string OpToString(const Op& op) {
  switch (op.index()) {
    case Op::GetTypeIndex<Eq>():
      return "Eq";
    case Op::GetTypeIndex<Ne>():
      return "Ne";
    case Op::GetTypeIndex<Lt>():
      return "Lt";
    case Op::GetTypeIndex<Le>():
      return "Le";
    case Op::GetTypeIndex<Gt>():
      return "Gt";
    case Op::GetTypeIndex<Ge>():
      return "Ge";
    case Op::GetTypeIndex<Glob>():
      return "Glob";
    case Op::GetTypeIndex<Regex>():
      return "Regex";
    default:
      PERFETTO_FATAL("Unknown op");
  }
}

std::string ResultToString(const CastFilterValueResult& res) {
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
      case base::variant_index<CastFilterValueResult::Value, const char*>(): {
        return base::unchecked_get<const char*>(res.value);
      }
      default:
        PERFETTO_FATAL("Unknown filter value type");
    }
  }
  return res.validity == CastFilterValueResult::Validity::kNoneMatch
             ? "NoneMatch"
             : "AllMatch";
}

template <typename T>
Span<T> GetSpan(std::vector<T>& vec) {
  return Span<T>{vec.data(), vec.data() + vec.size()};
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

class BytecodeInterpreterTest : public testing::Test {
 protected:
  template <typename... Ts>
  void SetRegistersAndExecute(const std::string& bytecode, Ts... value) {
    BytecodeVector bytecode_vector;
    bytecode_vector.emplace_back(ParseBytecode(bytecode));
    interpreter_ = std::make_unique<Interpreter<Fetcher>>(
        std::move(bytecode_vector), column_.get(), &spool_);
    uint32_t i = 0;
    (interpreter_->SetRegisterValueForTesting(reg::WriteHandle<Ts>(i++),
                                              std::move(value)),
     ...);
    interpreter_->Execute(fetcher_);
  }

  template <typename T>
  const T& GetRegister(uint32_t reg_idx) {
    const auto* r = interpreter_->GetRegisterValue(reg::ReadHandle<T>(reg_idx));
    PERFETTO_CHECK(r);
    return *r;
  }

  Fetcher fetcher_;
  StringPool spool_;
  std::unique_ptr<Column> column_;
  std::unique_ptr<Interpreter<Fetcher>> interpreter_;
};

TEST_F(BytecodeInterpreterTest, InitRange) {
  SetRegistersAndExecute("InitRange: [size=134, dest_register=Register(0)]");

  const auto& result = GetRegister<Range>(0);
  EXPECT_EQ(result.b, 0u);
  EXPECT_EQ(result.e, 134u);
}

TEST_F(BytecodeInterpreterTest, AllocateIndices) {
  SetRegistersAndExecute(
      "AllocateIndices: [size=132, dest_slab_register=Register(0), "
      "dest_span_register=Register(1)]");

  const auto& slab = GetRegister<Slab<uint32_t>>(0);
  { EXPECT_THAT(slab, SizeIs(132u)); }
  {
    const auto& span = GetRegister<Span<uint32_t>>(1);
    EXPECT_THAT(span, SizeIs(132u));
    EXPECT_EQ(span.b, slab.begin());
    EXPECT_EQ(span.e, slab.end());
  }
}

TEST_F(BytecodeInterpreterTest, AllocateIndicesAlreadyAllocated) {
  auto existing_slab = Slab<uint32_t>::Alloc(132u);
  auto* expected_begin = existing_slab.begin();
  auto* expected_end = existing_slab.end();
  SetRegistersAndExecute(
      "AllocateIndices: [size=132, dest_slab_register=Register(0), "
      "dest_span_register=Register(1)]",
      std::move(existing_slab));

  const auto& slab = GetRegister<Slab<uint32_t>>(0);
  {
    EXPECT_EQ(slab.begin(), expected_begin);
    EXPECT_EQ(slab.end(), expected_end);
  }
  {
    const auto& span = GetRegister<Span<uint32_t>>(1);
    EXPECT_THAT(span, SizeIs(132u));
    EXPECT_EQ(span.b, slab.begin());
    EXPECT_EQ(span.e, slab.end());
  }
}

TEST_F(BytecodeInterpreterTest, Iota) {
  std::vector<uint32_t> res(132u);
  SetRegistersAndExecute(
      "Iota: [source_register=Register(0), update_register=Register(1)]",
      Range{5, 10}, GetSpan(res));

  const auto& update = GetRegister<Span<uint32_t>>(1);
  ASSERT_THAT(update.b, AllOf(testing::Ge(res.data()),
                              testing::Le(res.data() + res.size())));
  ASSERT_THAT(update.e, AllOf(testing::Ge(res.data()),
                              testing::Le(res.data() + res.size())));
  EXPECT_THAT(update, ElementsAreArray({5u, 6u, 7u, 8u, 9u}));
}

using CastResult = CastFilterValueResult;

struct CastTestCase {
  std::string input_type;
  FilterValue input;
  CastResult expected;
  Op op = dataframe::Eq{};
  static std::string ToString(const testing::TestParamInfo<CastTestCase>& i) {
    return ValToString(i.param.input) + "_" + ResultToString(i.param.expected) +
           "_" + OpToString(i.param.op);
  }
};

class BytecodeInterpreterCastTest
    : public BytecodeInterpreterTest,
      public testing::WithParamInterface<CastTestCase> {};

TEST_P(BytecodeInterpreterCastTest, Cast) {
  const auto& [input_type, input, expected, op] = GetParam();

  fetcher_.value = input;
  SetRegistersAndExecute(
      base::StackString<1024>(
          "CastFilterValue<%s>: [fval_handle=FilterValue(0), "
          "write_register=Register(0), op=Op(%u)]",
          input_type.c_str(), op.index())
          .ToStdString());

  const auto& result = GetRegister<CastFilterValueResult>(0);
  ASSERT_THAT(result.validity, testing::Eq(expected.validity));
  if (result.validity == CastResult::Validity::kValid) {
    if (std::holds_alternative<const char*>(expected.value)) {
      ASSERT_TRUE(std::holds_alternative<const char*>(result.value));
      ASSERT_STREQ(base::unchecked_get<const char*>(result.value),
                   base::unchecked_get<const char*>(expected.value));
    } else {
      ASSERT_THAT(result.value, testing::Eq(expected.value));
    }
    ASSERT_THAT(result.value, testing::Eq(expected.value));
  }
}

INSTANTIATE_TEST_SUITE_P(
    ToDouble,
    BytecodeInterpreterCastTest,
    testing::Values(
        CastTestCase{
            "Double",
            FilterValue{1024.0},
            CastResult::Valid(1024.0),
        },
        CastTestCase{
            "Double",
            FilterValue{1024l},
            CastResult::Valid(1024.0),
        },
        CastTestCase{
            "Double",
            FilterValue{int64_t(std::numeric_limits<int64_t>::max()) - 1},
            CastResult::NoneMatch(),
        },
        CastTestCase{
            "Double",
            FilterValue{int64_t(std::numeric_limits<int64_t>::max()) - 1},
            CastResult::AllMatch(),
            dataframe::Ne{},
        },
        CastTestCase{
            "Double",
            FilterValue{int64_t(std::numeric_limits<int64_t>::max()) - 1},
            CastResult::Valid(9223372036854775808.0),
            dataframe::Ge{},
        },
        CastTestCase{
            "Double",
            FilterValue{int64_t(std::numeric_limits<int64_t>::max()) - 1},
            CastResult::Valid(9223372036854774784.0),
            dataframe::Gt{},
        },
        CastTestCase{
            "Double",
            FilterValue{int64_t(std::numeric_limits<int64_t>::max()) - 1},
            CastResult::Valid(9223372036854775808.0),
            dataframe::Lt{},
        },
        CastTestCase{
            "Double",
            FilterValue{int64_t(std::numeric_limits<int64_t>::max()) - 1},
            CastResult::Valid(9223372036854774784.0),
            dataframe::Le{},
        },
        CastTestCase{
            "Double",
            FilterValue{9223372036854767615},
            CastResult::Valid(9223372036854767616.0),
            dataframe::Ge{},
        },
        CastTestCase{
            "Double",
            FilterValue{9223372036854767615},
            CastResult::Valid(9223372036854766592.0),
            dataframe::Gt{},
        },
        CastTestCase{
            "Double",
            FilterValue{9223372036854767615},
            CastResult::Valid(9223372036854767616.0),
            dataframe::Lt{},
        },
        CastTestCase{
            "Double",
            FilterValue{9223372036854767615},
            CastResult::Valid(9223372036854766592.0),
            dataframe::Le{},
        }),
    &CastTestCase::ToString);

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
            "Uint32",
            FilterValue{int64_t(std::numeric_limits<uint32_t>::max()) + 1},
            CastResult::NoneMatch(),
            dataframe::Ge{},
        },
        CastTestCase{
            "Uint32",
            FilterValue{int64_t(std::numeric_limits<uint32_t>::max()) + 1},
            CastResult::AllMatch(),
            dataframe::Le{},
        },
        CastTestCase{
            "Uint32",
            FilterValue{int64_t(std::numeric_limits<uint32_t>::min()) - 1},
            CastResult::AllMatch(),
            dataframe::Gt{},
        },
        CastTestCase{
            "Uint32",
            FilterValue{int64_t(std::numeric_limits<uint32_t>::min()) - 1},
            CastResult::NoneMatch(),
            dataframe::Lt{},
        },
        CastTestCase{
            "Uint32",
            FilterValue{int64_t(std::numeric_limits<uint32_t>::min()) - 1},
            CastResult::AllMatch(),
            dataframe::Ne{},
        },
        CastTestCase{
            "Int64",
            FilterValue{std::numeric_limits<int64_t>::max()},
            CastResult::Valid(std::numeric_limits<int64_t>::max()),
        }),
    &CastTestCase::ToString);

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
        },
        CastTestCase{
            "Int64",
            FilterValue{9223372036854775808.0},
            CastResult::AllMatch(),
            dataframe::Ne{},
        },
        CastTestCase{
            "Uint32",
            FilterValue{double(std::numeric_limits<uint32_t>::max()) - 0.5},
            CastResult::Valid(uint32_t(std::numeric_limits<uint32_t>::max() -
                                       1)),
            dataframe::Le{},
        },
        CastTestCase{
            "Uint32",
            FilterValue{double(std::numeric_limits<uint32_t>::max()) - 0.5},
            CastResult::Valid(uint32_t(std::numeric_limits<uint32_t>::max())),
            dataframe::Lt{},
        },
        CastTestCase{
            "Int32",
            FilterValue{double(std::numeric_limits<int32_t>::max()) - 0.5},
            CastResult::Valid(int32_t(std::numeric_limits<int32_t>::max())),
            dataframe::Ge{},
        },
        CastTestCase{
            "Int32",
            FilterValue{double(std::numeric_limits<int32_t>::max()) - 0.5},
            CastResult::Valid(int32_t(std::numeric_limits<int32_t>::max() - 1)),
            dataframe::Gt{},
        }),
    &CastTestCase::ToString);

INSTANTIATE_TEST_SUITE_P(
    CastToStringSuite,
    BytecodeInterpreterCastTest,
    testing::Values(
        // Strings are directly returned without any conversion.
        CastTestCase{"String", FilterValue{"hello"}, CastResult::Valid("hello"),
                     dataframe::Eq{}},
        CastTestCase{"String", FilterValue{"world"}, CastResult::Valid("world"),
                     dataframe::Ne{}},
        CastTestCase{"String", FilterValue{"test"}, CastResult::Valid("test"),
                     dataframe::Glob{}},
        CastTestCase{"String", FilterValue{"regex"}, CastResult::Valid("regex"),
                     dataframe::Regex{}},

        // Nulls always compare false with everything.
        CastTestCase{"String", FilterValue{nullptr}, CastResult::NoneMatch(),
                     dataframe::Eq{}},
        CastTestCase{"String", FilterValue{nullptr}, CastResult::NoneMatch(),
                     dataframe::Ne{}},
        CastTestCase{"String", FilterValue{nullptr}, CastResult::NoneMatch(),
                     dataframe::Lt{}},
        CastTestCase{"String", FilterValue{nullptr}, CastResult::NoneMatch(),
                     dataframe::Glob{}},
        CastTestCase{"String", FilterValue{nullptr}, CastResult::NoneMatch(),
                     dataframe::Regex{}},

        // Strings are always greater than integers.
        CastTestCase{"String", FilterValue{123l}, CastResult::AllMatch(),
                     dataframe::Eq{}},
        CastTestCase{"String", FilterValue{123l}, CastResult::AllMatch(),
                     dataframe::Ne{}},
        CastTestCase{"String", FilterValue{123l}, CastResult::NoneMatch(),
                     dataframe::Lt{}},
        CastTestCase{"String", FilterValue{123l}, CastResult::NoneMatch(),
                     dataframe::Le{}},
        CastTestCase{"String", FilterValue{123l}, CastResult::AllMatch(),
                     dataframe::Gt{}},
        CastTestCase{"String", FilterValue{123l}, CastResult::AllMatch(),
                     dataframe::Ge{}},
        CastTestCase{"String", FilterValue{123l}, CastResult::NoneMatch(),
                     dataframe::Glob{}},
        CastTestCase{"String", FilterValue{123l}, CastResult::NoneMatch(),
                     dataframe::Regex{}},

        // Strings are also always greater than doubles.
        CastTestCase{"String", FilterValue{123.45}, CastResult::AllMatch(),
                     dataframe::Eq{}},
        CastTestCase{"String", FilterValue{123.45}, CastResult::AllMatch(),
                     dataframe::Ne{}},
        CastTestCase{"String", FilterValue{123.45}, CastResult::NoneMatch(),
                     dataframe::Lt{}},
        CastTestCase{"String", FilterValue{123.45}, CastResult::NoneMatch(),
                     dataframe::Le{}},
        CastTestCase{"String", FilterValue{123.45}, CastResult::AllMatch(),
                     dataframe::Gt{}},
        CastTestCase{"String", FilterValue{123.45}, CastResult::AllMatch(),
                     dataframe::Ge{}},
        CastTestCase{"String", FilterValue{123.45}, CastResult::NoneMatch(),
                     dataframe::Glob{}},
        CastTestCase{"String", FilterValue{123.45}, CastResult::NoneMatch(),
                     dataframe::Regex{}}),
    &CastTestCase::ToString);

TEST_F(BytecodeInterpreterTest, SortedFilterIdEq) {
  std::string bytecode =
      "SortedFilter<Id, EqualRange>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(0)]";
  {
    // Test case 1: Value exists in range
    SetRegistersAndExecute(
        bytecode, CastFilterValueResult::Valid(CastFilterValueResult::Id{5}),
        Range{0, 10});

    const auto& result = GetRegister<Range>(1);
    EXPECT_EQ(result.b, 5u);
    EXPECT_EQ(result.e, 6u);
  }
  {
    // Test case 2: Value below range
    SetRegistersAndExecute(
        bytecode, CastFilterValueResult::Valid(CastFilterValueResult::Id{2}),
        Range{3, 10});
    EXPECT_THAT(GetRegister<Range>(1), IsEmpty());
  }
  {
    // Test case 3: Invalid cast result (NoneMatch)
    SetRegistersAndExecute(bytecode, CastFilterValueResult::NoneMatch(),
                           Range{0, 10});
    EXPECT_THAT(GetRegister<Range>(1), IsEmpty());
  }
}

TEST_F(BytecodeInterpreterTest, SortedFilterIdLowerBound) {
  std::string bytecode =
      "SortedFilter<Id, LowerBound>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(1)]";
  SetRegistersAndExecute(
      bytecode, CastFilterValueResult::Valid(CastFilterValueResult::Id{5}),
      Range{0, 10});

  const auto& result = GetRegister<Range>(1);
  EXPECT_EQ(result.b, 5u);
  EXPECT_EQ(result.e, 10u);
}

TEST_F(BytecodeInterpreterTest, SortedFilterIdUpperBound) {
  std::string bytecode =
      "SortedFilter<Id, UpperBound>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(2)]";
  SetRegistersAndExecute(
      bytecode, CastFilterValueResult::Valid(CastFilterValueResult::Id{5}),
      Range{0, 10});

  const auto& result = GetRegister<Range>(1);
  EXPECT_EQ(result.b, 0u);
  EXPECT_EQ(result.e, 6u);
}

TEST_F(BytecodeInterpreterTest, SortedFilterUint32Eq) {
  std::string bytecode =
      "SortedFilter<Uint32, EqualRange>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(0)]";

  auto values =
      CreateFlexVectorForTesting<uint32_t>({0u, 4u, 5u, 5u, 5u, 6u, 10u, 10u});
  column_.reset(new Column{ColumnSpec{"foo", Uint32(), Sorted(), NonNull()},
                           Storage{std::move(values)},
                           Overlay{Overlay::NoOverlay{}}});
  {
    // Test case 1: Value exists in range
    SetRegistersAndExecute(bytecode, CastFilterValueResult::Valid(5u),
                           Range{3u, 8u});
    const auto& result = GetRegister<Range>(1);
    EXPECT_EQ(result.b, 3u);
    EXPECT_EQ(result.e, 5u);
  }
  {
    // Test case 2: Value exists not range
    SetRegistersAndExecute(bytecode, CastFilterValueResult::Valid(4u),
                           Range{3u, 8u});
    EXPECT_THAT(GetRegister<Range>(1), IsEmpty());
  }
  {
    // Test case 3: Invalid cast result (NoneMatch)
    SetRegistersAndExecute(bytecode, CastFilterValueResult::NoneMatch(),
                           Range{0, 8u});
    EXPECT_THAT(GetRegister<Range>(1), IsEmpty());
  }
}

TEST_F(BytecodeInterpreterTest, SortedFilterUint32LowerBound) {
  std::string bytecode =
      "SortedFilter<Uint32, LowerBound>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(2)]";

  auto values =
      CreateFlexVectorForTesting<uint32_t>({0u, 4u, 5u, 5u, 5u, 6u, 10u, 10u});
  column_.reset(new Column{ColumnSpec{"foo", Uint32(), Sorted(), NonNull()},
                           Storage{std::move(values)},
                           Overlay{Overlay::NoOverlay{}}});

  SetRegistersAndExecute(bytecode, CastFilterValueResult::Valid(5u),
                         Range{3u, 8u});
  EXPECT_THAT(GetRegister<Range>(1), IsEmpty());

  SetRegistersAndExecute(bytecode, CastFilterValueResult::Valid(5u),
                         Range{1u, 8u});
  auto result = GetRegister<Range>(1);
  EXPECT_EQ(result.b, 1u);
  EXPECT_EQ(result.e, 2u);
}

TEST_F(BytecodeInterpreterTest, SortedFilterUint32UpperBound) {
  std::string bytecode =
      "SortedFilter<Uint32, UpperBound>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(1)]";

  auto values =
      CreateFlexVectorForTesting<uint32_t>({0u, 4u, 5u, 5u, 5u, 6u, 10u, 10u});
  column_.reset(new Column{ColumnSpec{"foo", Uint32(), Sorted(), NonNull()},
                           Storage{std::move(values)},
                           Overlay{Overlay::NoOverlay{}}});

  SetRegistersAndExecute(bytecode, CastFilterValueResult::Valid(5u),
                         Range{3u, 7u});
  auto result = GetRegister<Range>(1);
  EXPECT_EQ(result.b, 5u);
  EXPECT_EQ(result.e, 7u);
}

TEST_F(BytecodeInterpreterTest, FilterIdEq) {
  std::string bytecode =
      "NonStringFilter<Id, Eq>: [col=0, val_register=Register(0), "
      "source_register=Register(1), update_register=Register(2)]";

  std::vector<uint32_t> indices_spec = {12, 44, 10, 4, 5, 2, 3};
  {
    // Test case 1: Value exists in range
    std::vector<uint32_t> indices = indices_spec;
    SetRegistersAndExecute(
        bytecode, CastFilterValueResult::Valid(CastFilterValueResult::Id{5}),
        GetSpan(indices), GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), ElementsAre(5u));
  }
  {
    // Test case 2: Value above range
    std::vector<uint32_t> indices = indices_spec;
    SetRegistersAndExecute(
        bytecode, CastFilterValueResult::Valid(CastFilterValueResult::Id{11}),
        GetSpan(indices), GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
  }
  {
    // Test case 3: Invalid cast result (NoneMatch)
    std::vector<uint32_t> indices = indices_spec;
    SetRegistersAndExecute(bytecode, CastFilterValueResult::NoneMatch(),
                           CastFilterValueResult::NoneMatch(), GetSpan(indices),
                           GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
  }
}

TEST_F(BytecodeInterpreterTest, FilterUint32Eq) {
  std::string bytecode =
      "NonStringFilter<Uint32, Eq>: [col=0, val_register=Register(0), "
      "source_register=Register(1), update_register=Register(2)]";

  auto values =
      CreateFlexVectorForTesting<uint32_t>({4u, 49u, 392u, 4u, 49u, 4u, 391u});
  column_.reset(new Column{ColumnSpec{"foo", Uint32(), Unsorted(), NonNull()},
                           Storage{std::move(values)},
                           Overlay{Overlay::NoOverlay{}}});

  std::vector<uint32_t> indices_spec = {3, 3, 4, 5, 0, 6, 0};
  {
    // Test case 1: Value exists
    std::vector<uint32_t> indices = indices_spec;
    SetRegistersAndExecute(bytecode, CastFilterValueResult::Valid(4u),
                           GetSpan(indices), GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2),
                ElementsAre(3u, 3u, 5u, 0u, 0u));
  }
  {
    // Test case 2: Value does not exist
    std::vector<uint32_t> indices = indices_spec;
    SetRegistersAndExecute(bytecode, CastFilterValueResult::Valid(5u),
                           GetSpan(indices), GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
  }
  {
    // Test case 3: Invalid cast result (NoneMatch)
    std::vector<uint32_t> indices = indices_spec;
    SetRegistersAndExecute(bytecode, CastFilterValueResult::NoneMatch(),
                           GetSpan(indices), GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
  }
}

TEST_F(BytecodeInterpreterTest, StrideCopy) {
  std::string bytecode =
      "StrideCopy: [source_register=Register(0), update_register=Register(1), "
      "stride=3]";

  std::vector<uint32_t> source = {10, 3, 12, 4};
  std::vector<uint32_t> dest(source.size() * 3);
  SetRegistersAndExecute(bytecode, GetSpan(source), GetSpan(dest));

  EXPECT_THAT(GetRegister<Span<uint32_t>>(1),
              ElementsAre(10u, 0u, 0u, 3u, 0u, 0u, 12u, 0u, 0u, 4u, 0u, 0u));
}

TEST_F(BytecodeInterpreterTest, SortedFilterString) {
  auto apple_id = spool_.InternString("apple");
  auto banana_id = spool_.InternString("banana");
  auto cherry_id = spool_.InternString("cherry");
  auto date_id = spool_.InternString("date");

  // Sorted string data: ["apple", "banana", "banana", "cherry", "date"]
  auto values = CreateFlexVectorForTesting<StringPool::Id>(
      {apple_id, banana_id, banana_id, cherry_id, date_id});
  column_.reset(new Column{ColumnSpec{"col", String{}, Sorted{}, NonNull{}},
                           Storage{std::move(values)},
                           Overlay{Overlay::NoOverlay{}}});

  // --- Sub-test for EqualRange (Eq) ---
  {
    std::string bytecode_str =
        "SortedFilter<String, EqualRange>: [col=0, val_register=Register(0), "
        "update_register=Register(1), write_result_to=BoundModifier(0)]";
    SetRegistersAndExecute(bytecode_str, CastFilterValueResult::Valid("banana"),
                           Range{0, 5});
    const auto& result_range = GetRegister<Range>(1);
    EXPECT_EQ(result_range.b, 1u) << "EqualRange begin";
    EXPECT_EQ(result_range.e, 3u) << "EqualRange end";
  }
  // --- Sub-test for LowerBound (using Ge case) ---
  {
    // BoundModifier(1) == BeginBound (for Ge)
    std::string bytecode_str =
        "SortedFilter<String, LowerBound>: [col=0, val_register=Register(0), "
        "update_register=Register(1), write_result_to=BoundModifier(1)]";
    SetRegistersAndExecute(bytecode_str, CastFilterValueResult::Valid("banana"),
                           Range{0, 5});
    const auto& result_range = GetRegister<Range>(1);
    EXPECT_EQ(result_range.b, 1u) << "LowerBound(Ge) begin";
    EXPECT_EQ(result_range.e, 5u) << "LowerBound(Ge) end";
  }
  // --- Sub-test for UpperBound (using Le case) ---
  {
    // BoundModifier(2) == EndBound (for Le)
    std::string bytecode_str =
        "SortedFilter<String, UpperBound>: [col=0, val_register=Register(0), "
        "update_register=Register(1), write_result_to=BoundModifier(2)]";
    SetRegistersAndExecute(bytecode_str, CastFilterValueResult::Valid("banana"),
                           Range{0, 5});
    const auto& result_range = GetRegister<Range>(1);
    EXPECT_EQ(result_range.b, 0u) << "UpperBound(Le) begin";
    EXPECT_EQ(result_range.e, 3u) << "UpperBound(Le) end";
  }
}

TEST_F(BytecodeInterpreterTest, StringFilter) {
  // 1. Setup Shared Column Data (Unsorted, includes empty string)
  auto apple_id = spool_.InternString("apple");
  auto banana_id = spool_.InternString("banana");
  auto cherry_id = spool_.InternString("cherry");
  auto date_id = spool_.InternString("date");
  auto durian_id = spool_.InternString("durian");
  auto empty_id = spool_.InternString("");  // Intern the empty string

  // Data: ["cherry", "apple", "", "banana", "apple", "date", "durian"]
  // Index:    0        1      2      3        4       5        6
  auto values = CreateFlexVectorForTesting<StringPool::Id>(
      {cherry_id, apple_id, empty_id, banana_id, apple_id, date_id, durian_id});
  column_.reset(new Column{ColumnSpec{"col", String{}, Unsorted{}, NonNull{}},
                           Storage{std::move(values)},
                           Overlay{Overlay::NoOverlay{}}});

  // Initial indices {0, 1, 2, 3, 4, 5, 6} pointing to the data
  const std::vector<uint32_t> source_indices = {0, 1, 2, 3, 4, 5, 6};

  // 2. Define Helper Lambda for Running Sub-Tests
  auto RunStringFilterSubTest =
      [&](const std::string& test_label, const std::string& op_name,
          const char* filter_value,
          const std::vector<uint32_t>& expected_indices) {
        // Construct the bytecode string for the specific operation
        std::string bytecode_str =
            base::StackString<1024>(
                "StringFilter<%s>: [col=0, val_register=Register(0), "
                "source_register=Register(1), update_register=Register(2)]",
                op_name.c_str())
                .ToStdString();

        std::vector<uint32_t> res = source_indices;
        SetRegistersAndExecute(bytecode_str,
                               CastFilterValueResult::Valid(filter_value),
                               GetSpan(res), GetSpan(res));
        EXPECT_THAT(GetRegister<Span<uint32_t>>(2),
                    ElementsAreArray(expected_indices))
            << test_label;
      };

  RunStringFilterSubTest("Eq apple", "Eq", "apple", {1, 4});
  RunStringFilterSubTest("Ne apple", "Ne", "apple", {0, 2, 3, 5, 6});
  RunStringFilterSubTest("Glob a*e", "Glob", "a*e", {1, 4});  // Matches apple
  RunStringFilterSubTest("Regex ^d", "Regex", "^d",
                         {5, 6});  // Matches date, durian
  RunStringFilterSubTest("Lt banana", "Lt", "banana",
                         {1, 2, 4});  // Matches apple, ""
  RunStringFilterSubTest("Ge cherry", "Ge", "cherry",
                         {0, 5, 6});  // Matches cherry, date, durian
  RunStringFilterSubTest("Le banana", "Le", "banana",
                         {1, 2, 3, 4});  // apple, "", banana, apple
  RunStringFilterSubTest("Gt cherry", "Gt", "cherry", {5, 6});  // date, durian

  RunStringFilterSubTest("Glob 'apple' as Eq", "Glob", "apple", {1, 4});
  RunStringFilterSubTest("Eq empty string", "Eq", "", {2});
  RunStringFilterSubTest("Eq string not in pool", "Eq", "grape", {});
  RunStringFilterSubTest("Ne empty string", "Ne", "", {0, 1, 3, 4, 5, 6});
  RunStringFilterSubTest("Ne string not in pool", "Ne", "grape",
                         {0, 1, 2, 3, 4, 5, 6});
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe::impl::bytecode
