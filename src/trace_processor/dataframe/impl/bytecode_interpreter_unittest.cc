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
#include <numeric>
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
#include "src/trace_processor/dataframe/impl/bit_vector.h"
#include "src/trace_processor/dataframe/impl/bytecode_core.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/flex_vector.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/value_fetcher.h"
#include "src/trace_processor/util/regex.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe::impl::bytecode {
namespace {

using testing::AllOf;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::IsEmpty;
using testing::SizeIs;
using testing::UnorderedElementsAre;

using FilterValue = std::variant<int64_t, double, const char*, std::nullptr_t>;

struct Fetcher : ValueFetcher {
  using Type = size_t;
  static constexpr Type kInt64 = base::variant_index<FilterValue, int64_t>();
  static constexpr Type kDouble = base::variant_index<FilterValue, double>();
  static constexpr Type kString =
      base::variant_index<FilterValue, const char*>();
  static constexpr Type kNull =
      base::variant_index<FilterValue, std::nullptr_t>();

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
    case base::variant_index<FilterValue, std::nullptr_t>():
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
  static constexpr std::array<std::array<uint32_t, 8>, kNumBytecodeCount>
      offsets{PERFETTO_DATAFRAME_BYTECODE_LIST(
          PERFETTO_DATAFRAME_BYTECODE_OFFSETS)};

#define PERFETTO_DATAFRAME_BYTECODE_NAMES(...) __VA_ARGS__::kNames,
  static constexpr std::array<std::array<const char*, 7>, kNumBytecodeCount>
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
    size_t eq_pos = arg.find('=');
    PERFETTO_CHECK(eq_pos != std::string::npos);
    std::string arg_name = arg.substr(0, eq_pos);
    std::string arg_val = arg.substr(eq_pos + 1);

    // Remove everything before the first "(" (which may not be the first
    // character) and after the last ")".
    if (size_t open = arg_val.find('('); open != std::string_view::npos) {
      arg_val = arg_val.substr(open + 1, arg_val.rfind(')') - open - 1);
    }

    const auto& n = names[bc.option];
    const auto* it = std::find(n.data(), n.data() + n.size(), arg_name);
    PERFETTO_CHECK(it != n.data() + n.size());
    auto arg_idx = static_cast<uint32_t>(it - n.data());
    uint32_t size = cur_offset[arg_idx + 1] - cur_offset[arg_idx];
    if (size == 2) {
      auto val = base::StringToInt32(arg_val);
      PERFETTO_CHECK(val.has_value());
      auto cast = static_cast<uint16_t>(*val);
      memcpy(&bc.args_buffer[cur_offset[arg_idx]], &cast, 2);
    } else if (size == 4) {
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

template <typename T, typename U>
Column CreateNonNullUnsortedColumn(std::string name,
                                   std::initializer_list<U> data,
                                   StringPool* pool = nullptr) {
  impl::FlexVector<T> vec;
  if constexpr (std::is_same_v<T, StringPool::Id>) {
    PERFETTO_CHECK(pool);
    for (const auto& str_like : data) {
      vec.push_back(pool->InternString(str_like));
    }
  } else {
    for (const U& val : data) {
      vec.push_back(val);
    }
  }
  return impl::Column{std::move(name), impl::Storage{std::move(vec)},
                      impl::Overlay::NoOverlay{}, Unsorted{}};
}

template <typename T>
FlexVector<T> CreateFlexVectorForTesting(std::initializer_list<T> values) {
  FlexVector<T> vec;
  for (const auto& value : values) {
    vec.push_back(value);
  }
  return vec;
}

template <typename T>
Column CreateSparseNullableColumn(
    std::string name,
    const std::vector<std::optional<T>>& data_with_nulls,
    SortState sort_state = Unsorted{}) {
  auto num_rows = static_cast<uint32_t>(data_with_nulls.size());
  auto data_vec = FlexVector<T>::CreateWithCapacity(num_rows);
  auto bv = BitVector::CreateWithSize(num_rows);
  for (uint32_t i = 0; i < num_rows; ++i) {
    if (data_with_nulls[i].has_value()) {
      data_vec.push_back(*data_with_nulls[i]);
      bv.set(i);
    }
  }
  return impl::Column{std::move(name), impl::Storage{std::move(data_vec)},
                      impl::Overlay{impl::Overlay::SparseNull{std::move(bv)}},
                      sort_state};
}

Column CreateSparseNullableStringColumn(
    std::string name,
    const std::vector<std::optional<const char*>>& data_with_nulls,
    StringPool* pool,
    SortState sort_state = Unsorted{}) {
  auto num_rows = static_cast<uint32_t>(data_with_nulls.size());
  auto data_vec = FlexVector<StringPool::Id>::CreateWithCapacity(num_rows);
  auto bv = BitVector::CreateWithSize(num_rows);
  for (uint32_t i = 0; i < num_rows; ++i) {
    if (data_with_nulls[i].has_value()) {
      data_vec.push_back(pool->InternString(*data_with_nulls[i]));
      bv.set(i);
    }
  }
  return impl::Column{std::move(name), impl::Storage{std::move(data_vec)},
                      impl::Overlay{impl::Overlay::SparseNull{std::move(bv)}},
                      sort_state};
}

template <typename T>
Column CreateDenseNullableColumn(
    std::string name,
    const std::vector<std::optional<T>>& data_with_nulls,
    SortState sort_state = Unsorted{}) {
  auto num_rows = static_cast<uint32_t>(data_with_nulls.size());
  auto data_vec = FlexVector<T>::CreateWithSize(num_rows);
  auto bv = BitVector::CreateWithSize(num_rows);

  for (uint32_t i = 0; i < num_rows; ++i) {
    if (data_with_nulls[i].has_value()) {
      data_vec[i] = *data_with_nulls[i];
      bv.set(i);
    } else {
      data_vec[i] = T{};  // Default construct T for null storage slot
    }
  }
  return impl::Column{std::move(name), impl::Storage{std::move(data_vec)},
                      impl::Overlay{impl::Overlay::DenseNull{std::move(bv)}},
                      sort_state};
}

Column CreateDenseNullableStringColumn(
    std::string name,
    const std::vector<std::optional<const char*>>& data_with_nulls,
    StringPool* pool,
    SortState sort_state = Unsorted{}) {
  auto num_rows = static_cast<uint32_t>(data_with_nulls.size());
  auto data_vec = FlexVector<StringPool::Id>::CreateWithSize(num_rows);
  auto bv = BitVector::CreateWithSize(num_rows);

  for (uint32_t i = 0; i < num_rows; ++i) {
    if (data_with_nulls[i].has_value()) {
      data_vec[i] = pool->InternString(*data_with_nulls[i]);
      bv.set(i);
    } else {
      data_vec[i] = StringPool::Id::Null();
    }
  }
  return impl::Column{std::move(name), impl::Storage{std::move(data_vec)},
                      impl::Overlay{impl::Overlay::DenseNull{std::move(bv)}},
                      sort_state};
}

class BytecodeInterpreterTest : public testing::Test {
 protected:
  template <typename... Ts>
  void SetRegistersAndExecute(const std::string& bytecode_str, Ts... value) {
    BytecodeVector bytecode_vector;
    std::vector<std::string> lines = base::SplitString(bytecode_str, "\n");
    for (const auto& line : lines) {
      std::string trimmed = base::TrimWhitespace(line);
      if (!trimmed.empty()) {
        bytecode_vector.emplace_back(ParseBytecode(trimmed));
      }
    }
    SetupInterpreterWithBytecode(std::move(bytecode_vector));

    uint32_t i = 0;
    (interpreter_->SetRegisterValueForTesting(reg::WriteHandle<Ts>(i++),
                                              std::move(value)),
     ...);
    interpreter_->Execute(fetcher_);
  }

  void SetupInterpreterWithBytecode(BytecodeVector bytecode) {
    interpreter_ = std::make_unique<Interpreter<Fetcher>>(
        std::move(bytecode), columns_vec_.data(), &spool_);
  }

  template <typename T>
  const T& GetRegister(uint32_t reg_idx) {
    const auto* r = interpreter_->GetRegisterValue(reg::ReadHandle<T>(reg_idx));
    PERFETTO_CHECK(r);
    return *r;
  }

  Fetcher fetcher_;
  StringPool spool_;
  std::vector<Column> columns_vec_;
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
  {
    EXPECT_THAT(slab, SizeIs(132u));
  }
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
            FilterValue{int64_t(1024)},
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
            FilterValue{int64_t(9223372036854767615)},
            CastResult::Valid(9223372036854767616.0),
            dataframe::Ge{},
        },
        CastTestCase{
            "Double",
            FilterValue{int64_t(9223372036854767615)},
            CastResult::Valid(9223372036854766592.0),
            dataframe::Gt{},
        },
        CastTestCase{
            "Double",
            FilterValue{int64_t(9223372036854767615)},
            CastResult::Valid(9223372036854767616.0),
            dataframe::Lt{},
        },
        CastTestCase{
            "Double",
            FilterValue{int64_t(9223372036854767615)},
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
            FilterValue{int64_t(1024)},
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
            FilterValue{int64_t(1024)},
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

const char* kHello = "hello";
const char* kWorld = "world";
const char* kTest = "test";
const char* kRegex = "regex";

INSTANTIATE_TEST_SUITE_P(
    CastToStringSuite,
    BytecodeInterpreterCastTest,
    testing::Values(
        // Strings are directly returned without any conversion.
        CastTestCase{"String", FilterValue{kHello}, CastResult::Valid(kHello),
                     dataframe::Eq{}},
        CastTestCase{"String", FilterValue{kWorld}, CastResult::Valid(kWorld),
                     dataframe::Ne{}},
        CastTestCase{"String", FilterValue{kTest}, CastResult::Valid(kTest),
                     dataframe::Glob{}},
        CastTestCase{"String", FilterValue{kRegex}, CastResult::Valid(kRegex),
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
        CastTestCase{"String", FilterValue{int64_t(123)},
                     CastResult::AllMatch(), dataframe::Eq{}},
        CastTestCase{"String", FilterValue{int64_t(123)},
                     CastResult::AllMatch(), dataframe::Ne{}},
        CastTestCase{"String", FilterValue{int64_t(123)},
                     CastResult::NoneMatch(), dataframe::Lt{}},
        CastTestCase{"String", FilterValue{int64_t(123)},
                     CastResult::NoneMatch(), dataframe::Le{}},
        CastTestCase{"String", FilterValue{int64_t(123)},
                     CastResult::AllMatch(), dataframe::Gt{}},
        CastTestCase{"String", FilterValue{int64_t(123)},
                     CastResult::AllMatch(), dataframe::Ge{}},
        CastTestCase{"String", FilterValue{int64_t(123)},
                     CastResult::NoneMatch(), dataframe::Glob{}},
        CastTestCase{"String", FilterValue{int64_t(123)},
                     CastResult::NoneMatch(), dataframe::Regex{}},

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
  columns_vec_.emplace_back(
      impl::Column{"foo", std::move(values), Overlay::NoOverlay{}, Sorted{}});
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
  columns_vec_.emplace_back(impl::Column{"foo", Storage{std::move(values)},
                                         Overlay{Overlay::NoOverlay{}},
                                         Sorted{}});

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
  columns_vec_.emplace_back(
      impl::Column{"foo", std::move(values), Overlay::NoOverlay{}, Sorted{}});

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
  columns_vec_.emplace_back(
      impl::Column{"foo", std::move(values), Overlay::NoOverlay{}, Unsorted{}});

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
  columns_vec_.emplace_back(
      impl::Column{"foo", std::move(values), Overlay::NoOverlay{}, Sorted{}});

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
  columns_vec_.emplace_back(
      impl::Column{"foo", std::move(values), Overlay::NoOverlay{}, Unsorted{}});

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
  if constexpr (regex::IsRegexSupported()) {
    RunStringFilterSubTest("Regex ^d", "Regex", "^d",
                           {5, 6});  // Matches date, durian
  }
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
TEST_F(BytecodeInterpreterTest, NullFilter) {
  // Create a BitVector representing nulls: 0=null, 1=not_null, 2=null,
  // 3=not_null, ...
  //
  // Indices:    0  1   2  3   4  5    6   7   8   9  10 ... 63 64 65 66
  // Is Null:    T  F   T  F   T  F    T   F   T   F   T ...  F  T  F  T
  // Is Set (BV):F  T   F  T   F  T    F   T   F   T   F ...  T  F  T  F
  constexpr uint32_t kNumIndices = 70;
  auto bv = BitVector::CreateWithSize(kNumIndices);
  for (uint32_t i = 0; i < kNumIndices; ++i) {
    // Set bits for non-null indices (odd indices)
    if (i % 2 != 0) {
      bv.set(i);
    }
  }

  // Create a dummy column with a DenseNull overlay using the BitVector
  // (SparseNull would work identically for this specific test)
  columns_vec_.emplace_back(impl::Column{
      "foo",
      Storage{Storage::Uint32{}},  // Storage type doesn't matter for NullFilter
      Overlay{Overlay::DenseNull{std::move(bv)}}, Unsorted{}});

  std::vector<uint32_t> indices(kNumIndices);
  std::iota(indices.begin(), indices.end(), 0);
  {
    std::vector<uint32_t> res = indices;
    SetRegistersAndExecute(
        "NullFilter<IsNull>: [col=0, update_register=Register(0)]",
        GetSpan(res));

    // Expected output: indices where the bit was *not* set (even indices)
    std::vector<uint32_t> expected_isnull;
    for (uint32_t i = 0; i < kNumIndices; i += 2) {
      expected_isnull.push_back(i);
    }
    EXPECT_THAT(GetRegister<Span<uint32_t>>(0),
                ElementsAreArray(expected_isnull));
  }
  {
    std::vector<uint32_t> res = indices;
    SetRegistersAndExecute(
        "NullFilter<IsNotNull>: [col=0, update_register=Register(0)]",
        GetSpan(res));

    // Expected output: indices where the bit *was* set (odd indices)
    std::vector<uint32_t> expected_isnotnull;
    for (uint32_t i = 1; i < kNumIndices; i += 2) {
      expected_isnotnull.push_back(i);
    }
    EXPECT_THAT(GetRegister<Span<uint32_t>>(0),
                ElementsAreArray(expected_isnotnull));
  }
}

TEST_F(BytecodeInterpreterTest, PrefixPopcount) {
  // Create a BitVector with a specific pattern across words
  // Word 0 (0-63):   Bits 5, 20, 40 set (3 bits)
  // Word 1 (64-127): Bits 70, 100 set (2 bits)
  // Word 2 (128-191):Bits 130, 140, 150, 160 set (4 bits)
  // Word 3 (192-255):Bit 200 set (1 bit)
  constexpr uint32_t kNumBits = 210;
  auto bv = BitVector::CreateWithSize(kNumBits);
  bv.set(5);
  bv.set(20);
  bv.set(40);  // Word 0
  bv.set(70);
  bv.set(100);  // Word 1
  bv.set(130);
  bv.set(140);
  bv.set(150);
  bv.set(160);  // Word 2
  bv.set(200);  // Word 3

  columns_vec_.emplace_back(impl::Column{
      "foo", Storage{Storage::Uint32{}},  // Storage type doesn't matter
      Overlay{Overlay::SparseNull{std::move(bv)}}, Unsorted{}});
  SetRegistersAndExecute("PrefixPopcount: [col=0, dest_register=Register(0)]");

  const auto& result_slab = GetRegister<Slab<uint32_t>>(0);

  // Expected prefix sums:
  // Before word 0: 0
  // Before word 1: 0 + 3 = 3
  // Before word 2: 3 + 2 = 5
  // Before word 3: 5 + 4 = 9
  // Total words needed = ceil(210 / 64) = 4
  ASSERT_EQ(result_slab.size(), 4u);
  EXPECT_THAT(std::vector<uint32_t>(result_slab.begin(), result_slab.end()),
              ElementsAre(0u, 3u, 5u, 9u));

  // Execute again. The interpreter should detect the register is already
  // populated and not recompute.
  interpreter_->Execute(fetcher_);

  const auto& result_slab_cached = GetRegister<Slab<uint32_t>>(0);
  EXPECT_THAT(result_slab_cached, ElementsAre(0u, 3u, 5u, 9u));

  // Check that the underlying data pointer is the same, proving it wasn't
  // reallocated.
  EXPECT_EQ(result_slab_cached.data(), result_slab.data());
}

TEST_F(BytecodeInterpreterTest, TranslateSparseNullIndices) {
  // Use the same BitVector and PrefixPopcount setup as the PrefixPopcount test
  // Word 0 (0-63):    Bits 5, 20, 40 set (3 bits) -> Storage Indices 0, 1, 2
  // Word 1 (64-127):  Bits 70, 100 set (2 bits) -> Storage Indices 3, 4
  // Word 2 (128-191): Bits 130, 140, 150, 160 set (4 bits) -> Storage Indices
  //                   5, 6, 7, 8
  // Word 3 (192-255): Bit 200 set (1 bit) -> Storage Index 9
  constexpr uint32_t kNumBits = 210;
  auto bv = BitVector::CreateWithSize(kNumBits);
  bv.set(5);
  bv.set(20);
  bv.set(40);  // Word 0
  bv.set(70);
  bv.set(100);  // Word 1
  bv.set(130);
  bv.set(140);
  bv.set(150);
  bv.set(160);  // Word 2
  bv.set(200);  // Word 3

  columns_vec_.emplace_back(impl::Column{
      "foo", Storage{Storage::Uint32{}},  // Storage type doesn't matter
      Overlay{Overlay::SparseNull{std::move(bv)}}, Unsorted{}});

  // Precomputed PrefixPopcount Slab (from previous test)
  auto popcount_slab = Slab<uint32_t>::Alloc(4);
  popcount_slab[0] = 0;
  popcount_slab[1] = 3;
  popcount_slab[2] = 5;
  popcount_slab[3] = 9;

  std::vector<uint32_t> source_indices = {5, 40, 70, 150, 200};
  std::vector<uint32_t> translated_indices(source_indices.size());
  SetRegistersAndExecute(
      "TranslateSparseNullIndices: [col=0, popcount_register=Register(0), "
      "source_register=Register(1), update_register=Register(2)]",
      std::move(popcount_slab), GetSpan(source_indices),
      GetSpan(translated_indices));

  // Verify the translated indices in Register 2
  // Index 5 -> Storage 0 (Popcnt[0] + 0)
  // Index 40 -> Storage 2 (Popcnt[0] + 2)
  // Index 70 -> Storage 3 (Popcnt[1] + 0)
  // Index 150 -> Storage 7 (Popcnt[2] + 2)
  // Index 200 -> Storage 9 (Popcnt[3] + 0)
  EXPECT_THAT(GetRegister<Span<uint32_t>>(2), ElementsAre(0u, 2u, 3u, 7u, 9u));
}

TEST_F(BytecodeInterpreterTest, StrideTranslateAndCopySparseNullIndices) {
  // Use the same BitVector and PrefixPopcount setup as the PrefixPopcount test
  // Word 0 (0-63):    Bits 5, 20, 40 set (3 bits) -> Storage Indices 0, 1, 2
  // Word 1 (64-127):  Bits 70, 100 set (2 bits) -> Storage Indices 3, 4
  // Word 2 (128-191): Bits 130, 140, 150, 160 set (4 bits) -> Storage Indices
  //                   5, 6, 7, 8
  // Word 3 (192-255): Bit 200 set (1 bit) -> Storage Index 9
  constexpr uint32_t kNumBits = 210;
  auto bv = BitVector::CreateWithSize(kNumBits);
  bv.set(5);
  bv.set(20);
  bv.set(40);  // Word 0
  bv.set(70);
  bv.set(100);  // Word 1
  bv.set(130);
  bv.set(140);
  bv.set(150);
  bv.set(160);  // Word 2
  bv.set(200);  // Word 3

  // Precomputed PrefixPopcount Slab
  auto popcount_slab = Slab<uint32_t>::Alloc(4);
  popcount_slab[0] = 0;
  popcount_slab[1] = 3;
  popcount_slab[2] = 5;
  popcount_slab[3] = 9;

  // Create a dummy column with the BitVector (SparseNull overlay)
  columns_vec_.emplace_back(impl::Column{
      "foo", Storage{Storage::Uint32{}},  // Storage type doesn't matter
      Overlay{Overlay::SparseNull{std::move(bv)}}, Unsorted{}});

  // Input/Output buffer setup: Stride = 3, Offset for this column = 1
  // We pre-populate offset 0 with the original indices to simulate the state
  // after StrideCopy would have run.
  constexpr uint32_t kStride = 3;
  constexpr uint32_t kOffset = 1;
  std::vector<uint32_t> original_indices = {0, 5, 20, 64, 70, 130, 199, 200};
  std::vector<uint32_t> buffer(original_indices.size() * kStride,
                               999);  // Fill with dummy
  for (size_t i = 0; i < original_indices.size(); ++i) {
    buffer[(i * kStride) + 0] = original_indices[i];
  }

  SetRegistersAndExecute(
      "StrideTranslateAndCopySparseNullIndices: [col=0, "
      "popcount_register=Register(0), "
      "update_register=Register(1), offset=" +
          std::to_string(kOffset) + ", stride=" + std::to_string(kStride) + "]",
      std::move(popcount_slab), GetSpan(buffer));

  // Verify the contents of the buffer at the specified offset
  // Original Index | Is Set (Not Null) | Storage Index | Expected @ Offset 1
  // ----------------|-------------------|---------------|--------------------
  // 0               | F (Null)          | N/A           | UINT32_MAX
  // 5               | T (Not Null)      | 0             | 0
  // 20              | T (Not Null)      | 1             | 1
  // 64              | F (Null)          | N/A           | UINT32_MAX
  // 70              | T (Not Null)      | 3             | 3
  // 130             | T (Not Null)      | 5             | 5
  // 199             | F (Null)          | N/A           | UINT32_MAX
  // 200             | T (Not Null)      | 9             | 9
  const uint32_t N = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> expected_buffer = {
      0,   N, 999,  // Row 0 (Index 0 -> Null)
      5,   0, 999,  // Row 1 (Index 5 -> Storage 0)
      20,  1, 999,  // Row 2 (Index 20 -> Storage 1)
      64,  N, 999,  // Row 3 (Index 64 -> Null)
      70,  3, 999,  // Row 4 (Index 70 -> Storage 3)
      130, 5, 999,  // Row 5 (Index 130 -> Storage 5)
      199, N, 999,  // Row 6 (Index 199 -> Null)
      200, 9, 999   // Row 7 (Index 200 -> Storage 9)
  };
  EXPECT_THAT(GetRegister<Span<uint32_t>>(1),
              ElementsAreArray(expected_buffer));
}

TEST_F(BytecodeInterpreterTest, StrideCopyDenseNullIndices) {
  // Use the same BitVector setup as the PrefixPopcount test
  // Word 0 (0-63):   Bits 5, 20, 40 set
  // Word 1 (64-127): Bits 70, 100 set
  // Word 2 (128-191):Bits 130, 140, 150, 160 set
  // Word 3 (192-255):Bit 200 set
  constexpr uint32_t kNumBits = 210;
  auto bv = BitVector::CreateWithSize(kNumBits);
  bv.set(5);
  bv.set(20);
  bv.set(40);  // Word 0
  bv.set(70);
  bv.set(100);  // Word 1
  bv.set(130);
  bv.set(140);
  bv.set(150);
  bv.set(160);  // Word 2
  bv.set(200);  // Word 3

  // Create a dummy column with the BitVector (DenseNull overlay)
  columns_vec_.emplace_back(impl::Column{
      "foo", Storage{Storage::Uint32{}},  // Storage type doesn't matter
      Overlay{Overlay::DenseNull{std::move(bv)}}, Unsorted{}});

  // Input/Output buffer setup: Stride = 2, Offset for this column = 1
  // Pre-populate offset 0 with the original indices.
  constexpr uint32_t kStride = 2;
  constexpr uint32_t kOffset = 1;
  std::vector<uint32_t> original_indices = {0, 5, 20, 64, 70, 130, 199, 200};
  std::vector<uint32_t> buffer(original_indices.size() * kStride,
                               999);  // Fill with dummy
  for (size_t i = 0; i < original_indices.size(); ++i) {
    buffer[(i * kStride) + 0] = original_indices[i];  // Populate offset 0
  }

  SetRegistersAndExecute(
      "StrideCopyDenseNullIndices: [col=0, update_register=Register(0), "
      "offset=" +
          std::to_string(kOffset) + ", stride=" + std::to_string(kStride) + "]",
      GetSpan(buffer));

  // Verify the contents of the buffer at the specified offset
  // Original Index | Is Set (Not Null) | Expected @ Offset 1
  // ----------------|-------------------|--------------------
  // 0               | F (Null)          | UINT32_MAX
  // 5               | T (Not Null)      | 5
  // 20              | T (Not Null)      | 20
  // 64              | F (Null)          | UINT32_MAX
  // 70              | T (Not Null)      | 70
  // 130             | T (Not Null)      | 130
  // 199             | F (Null)          | UINT32_MAX
  // 200             | T (Not Null)      | 200
  const uint32_t N = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> expected_buffer = {
      0,   N,    // Row 0 (Index 0 -> Null)
      5,   5,    // Row 1 (Index 5 -> Not Null)
      20,  20,   // Row 2 (Index 20 -> Not Null)
      64,  N,    // Row 3 (Index 64 -> Null)
      70,  70,   // Row 4 (Index 70 -> Not Null)
      130, 130,  // Row 5 (Index 130 -> Not Null)
      199, N,    // Row 6 (Index 199 -> Null)
      200, 200   // Row 7 (Index 200 -> Not Null)
  };
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0),
              ElementsAreArray(expected_buffer));
}

// Test NonStringFilter simulating in-place filtering behavior.
// This happens when update_register is filtered based on data lookups
// using indices from source_register (e.g., filtering SparseNull columns
// after translation).
TEST_F(BytecodeInterpreterTest, NonStringFilterInPlace) {
  // Column data: {5, 10, 5, 15, 10, 20}
  auto values = CreateFlexVectorForTesting<uint32_t>({5, 10, 5, 15, 10, 20});
  columns_vec_.emplace_back(impl::Column{
      "foo", std::move(values), Overlay{Overlay::NoOverlay{}}, Unsorted{}});

  // Source indices (imagine these are translated storage indices for data
  // lookup).
  // Indices:         0   1   3   4   5
  // Data values:     5  10  15  10  20
  std::vector<uint32_t> source_indices = {0, 1, 3, 4, 5};

  // Update buffer containing the actual indices we want to filter *in-place*.
  std::vector<uint32_t> update_indices = {100, 101, 102, 103,
                                          104};  // Initial values

  SetRegistersAndExecute(
      "NonStringFilter<Uint32, Eq>: [col=0, val_register=Register(0), "
      "source_register=Register(1), update_register=Register(2)]",
      CastFilterValueResult::Valid(10u), GetSpan(source_indices),
      GetSpan(update_indices));

  // Verify the update register (Register 2) - it should be filtered in-place.
  // Iteration | Src Idx | Data | Update Idx | Compares? | Action
  // ----------|---------|------|------------|-----------|--------
  // 1         | 0       | 5    | 100        | False     | -
  // 2         | 1       | 10   | 101        | True      | W/r 101 to output[0]
  // 3         | 3       | 15   | 102        | False     | -
  // 4         | 4       | 10   | 103        | True      | W/r 103 to output[1]
  // 5         | 5       | 20   | 104        | False     | -
  EXPECT_THAT(GetRegister<Span<uint32_t>>(2),
              ElementsAre(101u, 103u));  // Indices 101 and 103 are kept
}

TEST_F(BytecodeInterpreterTest, Uint32SetIdSortedEq) {
  // Data conforming to SetIdSorted: `data[v] == v` for the first occurrence of
  // v. Index:  0  1  2  3  4  5  6  7  8  9  10 Value:  0  0  0  3  3  5  5  7
  // 7  7  10
  auto values = CreateFlexVectorForTesting<uint32_t>(
      {0u, 0u, 0u, 3u, 3u, 5u, 5u, 7u, 7u, 7u, 10u});
  columns_vec_.emplace_back(impl::Column{
      "foo", std::move(values), Overlay{Overlay::NoOverlay{}}, SetIdSorted{}});

  std::string bytecode =
      "Uint32SetIdSortedEq: [col=0, val_register=Register(0), "
      "update_register=Register(1)]";

  auto RunSubTest = [&](const std::string& label, Range initial_range,
                        uint32_t filter_val, Range expected_range) {
    SCOPED_TRACE("Sub-test: " + label);
    SetRegistersAndExecute(bytecode,
                           CastFilterValueResult::Valid(uint32_t(filter_val)),
                           initial_range);
    const auto& result = GetRegister<Range>(1);
    EXPECT_EQ(result.b, expected_range.b) << "Range begin mismatch";
    EXPECT_EQ(result.e, expected_range.e) << "Range end mismatch";
  };

  Range full_range{0, 11};  // Covers all data {0..10}

  // --- Test Cases ---
  RunSubTest("Value 3 found", full_range, 3u, Range{3, 5});
  RunSubTest("Value 0 found", full_range, 0u, Range{0, 3});
  RunSubTest("Value 7 found", full_range, 7u, Range{7, 10});
  RunSubTest("Value 5 found", full_range, 5u, Range{5, 7});
  RunSubTest("Value 10 found (at end)", full_range, 10u, Range{10, 11});

  // Values not present
  RunSubTest("Value 2 not found (gap)", full_range, 2u,
             Range{2, 2});  // Clamp starts at index 2, finds 0, breaks. end=2.
  RunSubTest("Value 4 not found (gap)", full_range, 4u,
             Range{4, 4});  // Clamp starts at index 4, finds 3, breaks. end=4.
  RunSubTest("Value 6 not found (gap)", full_range, 6u,
             Range{6, 6});  // Clamp starts at index 6, finds 5, breaks. end=6.
  RunSubTest("Value 8 not found (gap)", full_range, 8u,
             Range{8, 8});  // Clamp starts at index 8, finds 7, breaks. end=8.
  RunSubTest(
      "Value 11 not found (above)", full_range, 11u,
      Range{11,
            11});  // Clamp starts at index 11 (end), loop doesn't run. end=11.

  // Range subsets
  RunSubTest("Value 3 found (range starts mid-value)", Range{4, 11}, 3u,
             Range{4, 5});
  RunSubTest("Value 7 found (range ends mid-value)", Range{0, 9}, 7u,
             Range{7, 9});
  RunSubTest("Value 5 found (subset range exact)", Range{5, 7}, 5u,
             Range{5, 7});
  RunSubTest("Value 0 not found (range excludes)", Range{3, 11}, 0u,
             Range{3, 3});
  RunSubTest("Value 10 not found (range excludes)", Range{0, 10}, 10u,
             Range{10, 10});

  // Test with invalid cast results
  {
    SCOPED_TRACE("Sub-test: Invalid Cast (NoneMatch)");
    SetRegistersAndExecute(bytecode, CastFilterValueResult::NoneMatch(),
                           full_range);
    const auto& result = GetRegister<Range>(1);
    EXPECT_TRUE(result.empty());  // Should become empty
  }
  {
    SCOPED_TRACE("Sub-test: Invalid Cast (AllMatch)");
    SetRegistersAndExecute(bytecode, CastFilterValueResult::AllMatch(),
                           full_range);
    const auto& result = GetRegister<Range>(1);
    // Instruction returns early, keeps original range
    EXPECT_EQ(result.b, full_range.b);
    EXPECT_EQ(result.e, full_range.e);
  }
}

TEST_F(BytecodeInterpreterTest, ExecuteSortUint32Asc) {
  columns_vec_.emplace_back(
      CreateNonNullUnsortedColumn<uint32_t>("col", {50u, 10u, 30u, 20u, 40u}));

  std::vector<uint32_t> initial_indices = {0, 1, 2, 3, 4};
  SetRegistersAndExecute(
      "StableSortIndices<Uint32>: [col=0, direction=SortDirection(0), "
      "update_register=Register(0)]",
      GetSpan(initial_indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(1, 3, 2, 4, 0));
}

TEST_F(BytecodeInterpreterTest, ExecuteSortDoubleDesc) {
  columns_vec_.emplace_back(
      CreateNonNullUnsortedColumn<double>("col", {1.1, 5.5, 2.2, 4.4, 3.3}));

  std::vector<uint32_t> initial_indices = {0, 1, 2, 3, 4};
  SetRegistersAndExecute(
      "StableSortIndices<Double>: [col=0, direction=SortDirection(1), "
      "update_register=Register(0)]",
      GetSpan(initial_indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(1, 3, 4, 2, 0));
}

TEST_F(BytecodeInterpreterTest, ExecuteSortStringAsc) {
  columns_vec_.clear();
  columns_vec_.emplace_back(CreateNonNullUnsortedColumn<StringPool::Id>(
      "col", {"banana", "apple", "cherry", "date"}, &spool_));

  std::vector<uint32_t> initial_indices = {0, 1, 2, 3};
  SetRegistersAndExecute(
      "StableSortIndices<String>: [col=0, direction=SortDirection(0), "
      "update_register=Register(0)]",
      GetSpan(initial_indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(1, 0, 2, 3));
}

TEST_F(BytecodeInterpreterTest, ExecuteSortIdAsc) {
  columns_vec_.emplace_back(
      impl::Column{"id_col", impl::Storage{impl::Storage::Id{5}},
                   impl::Overlay::NoOverlay{}, IdSorted{}});

  std::vector<uint32_t> initial_indices = {3, 0, 4, 1, 2};
  SetRegistersAndExecute(
      "StableSortIndices<Id>: [col=0, direction=SortDirection(0), "
      "update_register=Register(0)]",
      GetSpan(initial_indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 1, 2, 3, 4));
}

TEST_F(BytecodeInterpreterTest, ExecuteStableSort) {
  columns_vec_.emplace_back(
      CreateNonNullUnsortedColumn<int64_t>("col_I", {10, 20, 10, 20, 10}));
  columns_vec_.emplace_back(CreateNonNullUnsortedColumn<StringPool::Id>(
      "col_S", {"c", "e", "a", "d", "b"}, &spool_));

  BytecodeVector bytecode;
  bytecode.emplace_back(ParseBytecode(
      "StableSortIndices<String>: [col=1, direction=SortDirection(1), "
      "update_register=Register(0)]"));
  bytecode.emplace_back(ParseBytecode(
      "StableSortIndices<Int64>: [col=0, direction=SortDirection(0), "
      "update_register=Register(0)]"));

  SetupInterpreterWithBytecode(std::move(bytecode));

  std::vector<uint32_t> initial_indices = {0, 1, 2, 3, 4};
  interpreter_->SetRegisterValueForTesting(reg::WriteHandle<Span<uint32_t>>(0),
                                           GetSpan(initial_indices));

  interpreter_->Execute(fetcher_);

  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 4, 2, 1, 3));
}

TEST_F(BytecodeInterpreterTest, ExecuteNullPartitionNullsAtStart) {
  auto data_vec = CreateFlexVectorForTesting<uint32_t>({100, 300, 400, 600});
  auto bv = BitVector::CreateWithSize(7);
  bv.set(1);
  bv.set(3);
  bv.set(4);
  bv.set(6);
  columns_vec_.push_back(impl::Column{
      "col", impl::Storage{std::move(data_vec)},
      impl::Overlay{impl::Overlay::SparseNull{std::move(bv)}}, Unsorted{}});

  std::vector<uint32_t> initial_indices = {0, 1, 2, 3, 4, 5, 6};
  SetRegistersAndExecute(
      "NullIndicesStablePartition: [col=0, nulls_location=NullsLocation(0), "
      "partition_register=Register(0), dest_non_null_register=Register(1)]",
      GetSpan(initial_indices), impl::Span<uint32_t>{nullptr, nullptr});

  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 2, 5, 1, 3, 4, 6));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(1), ElementsAre(1, 3, 4, 6));
}

TEST_F(BytecodeInterpreterTest, ExecuteNullPartitionNullsAtEnd) {
  auto data_vec = CreateFlexVectorForTesting<uint32_t>({100, 300, 400, 600});
  auto bv = BitVector::CreateWithSize(7);
  bv.set(1);
  bv.set(3);
  bv.set(4);
  bv.set(6);
  columns_vec_.push_back(impl::Column{
      "col", impl::Storage{std::move(data_vec)},
      impl::Overlay{impl::Overlay::SparseNull{std::move(bv)}}, Unsorted{}});

  std::vector<uint32_t> initial_indices = {0, 1, 2, 3, 4, 5, 6};
  SetRegistersAndExecute(
      "NullIndicesStablePartition: [col=0, nulls_location=NullsLocation(1), "
      "partition_register=Register(0), dest_non_null_register=Register(1)]",
      GetSpan(initial_indices), impl::Span<uint32_t>{nullptr, nullptr});

  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(1, 3, 4, 6, 0, 2, 5));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(1), ElementsAre(1, 3, 4, 6));
}

TEST_F(BytecodeInterpreterTest, ExecuteNullPartitionAllNulls) {
  auto data_vec = CreateFlexVectorForTesting<uint32_t>({});
  auto bv = BitVector::CreateWithSize(3);
  columns_vec_.push_back(impl::Column{
      "col", impl::Storage{std::move(data_vec)},
      impl::Overlay{impl::Overlay::SparseNull{std::move(bv)}}, Unsorted{}});

  std::vector<uint32_t> initial_indices = {0, 1, 2};
  SetRegistersAndExecute(
      "NullIndicesStablePartition: [col=0, nulls_location=NullsLocation(0), "
      "partition_register=Register(0), dest_non_null_register=Register(1)]",
      GetSpan(initial_indices), impl::Span<uint32_t>{nullptr, nullptr});

  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 1, 2));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(1), ElementsAre());
}

TEST_F(BytecodeInterpreterTest, ExecuteNullPartitionEmptyInput) {
  auto data_vec = CreateFlexVectorForTesting<uint32_t>({});
  auto bv = BitVector::CreateWithSize(0);
  columns_vec_.push_back(impl::Column{
      "col", impl::Storage{std::move(data_vec)},
      impl::Overlay{impl::Overlay::SparseNull{std::move(bv)}}, Unsorted{}});

  std::vector<uint32_t> initial_indices = {};
  SetRegistersAndExecute(
      "NullIndicesStablePartition: [col=0, nulls_location=NullsLocation(0), "
      "partition_register=Register(0), dest_non_null_register=Register(1)]",
      GetSpan(initial_indices), impl::Span<uint32_t>{nullptr, nullptr});

  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre());
  EXPECT_THAT(GetRegister<Span<uint32_t>>(1), ElementsAre());
}

TEST_F(BytecodeInterpreterTest, CopyToRowLayoutNonNull_Int32) {
  // Column: {100, 200, 300}
  columns_vec_.push_back(
      CreateNonNullUnsortedColumn<int32_t>("col_int", {100, 200, 300}));

  uint16_t copy_size = sizeof(int32_t);
  uint16_t stride = 8;
  uint16_t offset = 2;
  uint32_t num_rows = 3;
  uint32_t buffer_size = num_rows * stride;

  std::string bytecode_sequence = base::StackString<2048>(
                                      R"(
                                        AllocateRowLayoutBuffer: [buffer_size=%u, dest_buffer_register=Register(1)]
                                        CopyToRowLayoutNonNull: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(1), pad=0, row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
                                      )",
                                      buffer_size, offset, stride, copy_size)
                                      .ToStdString();

  std::vector<uint32_t> indices = {0, 1, 2};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));

  const auto& buffer = GetRegister<Slab<uint8_t>>(1);
  ASSERT_EQ(buffer.size(), buffer_size);

  int32_t expected_values[] = {100, 200, 300};
  for (uint32_t i = 0; i < num_rows; ++i) {
    int32_t actual_value;
    memcpy(&actual_value, buffer.data() + i * stride + offset, sizeof(int32_t));
    EXPECT_EQ(actual_value, expected_values[i])
        << "Mismatch at row index " << i;
  }
}

TEST_F(BytecodeInterpreterTest, CopyToRowLayoutDenseNull_String) {
  uint32_t num_rows = 5;
  columns_vec_.push_back(CreateDenseNullableStringColumn(
      "col_str", {"foo", std::nullopt, "bar", std::nullopt, "baz"}, &spool_));

  StringPool::Id foo_id = spool_.GetId("foo").value();
  StringPool::Id bar_id = spool_.GetId("bar").value();
  StringPool::Id baz_id = spool_.GetId("baz").value();

  uint16_t copy_size = sizeof(StringPool::Id);
  uint16_t stride = 1 + copy_size;
  uint16_t offset = 0;
  uint32_t buffer_size = num_rows * stride;

  std::string bytecode_sequence = base::StackString<2048>(
                                      R"(
                                        AllocateRowLayoutBuffer: [buffer_size=%u, dest_buffer_register=Register(1)]
                                        CopyToRowLayoutDenseNull: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(1), pad=0, row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
                                      )",
                                      buffer_size, offset, stride, copy_size)
                                      .ToStdString();

  std::vector<uint32_t> indices = {0, 1, 2, 3, 4};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));

  // Verification
  const auto& buffer = GetRegister<Slab<uint8_t>>(1);
  ASSERT_EQ(buffer.size(), buffer_size);

  struct ExpectedRow {
    bool non_null;
    StringPool::Id id;
  };
  ExpectedRow expected_data[] = {
      {true, foo_id}, {false, {}}, {true, bar_id}, {false, {}}, {true, baz_id}};

  for (uint32_t i = 0; i < num_rows; ++i) {
    const uint8_t* row_start = buffer.data() + i * stride;
    // Check null flag (at offset 0)
    uint8_t null_flag = row_start[offset];
    EXPECT_EQ(null_flag, static_cast<uint8_t>(expected_data[i].non_null))
        << "Null flag mismatch at row " << i;

    // Check data (at offset 0 + 1)
    StringPool::Id actual_id;
    memcpy(&actual_id, row_start + offset + 1, sizeof(StringPool::Id));
    if (expected_data[i].non_null) {
      EXPECT_EQ(actual_id, expected_data[i].id) << "Data mismatch at row " << i;
    } else {
      // Check if memory is zeroed for nulls
      std::vector<uint8_t> zeros(sizeof(StringPool::Id), 0);
      EXPECT_EQ(
          memcmp(row_start + offset + 1, zeros.data(), sizeof(StringPool::Id)),
          0)
          << "Null data not zeroed at row " << i;
    }
  }
}

TEST_F(BytecodeInterpreterTest, CopyToRowLayoutSparseNull_Int32) {
  // Column: {10, null, 30, null, 50} -> Non-null data {10, 30, 50}
  uint32_t num_rows = 5;
  columns_vec_.push_back(CreateSparseNullableColumn<int32_t>(
      "col_int", {10, std::nullopt, 30, std::nullopt, 50}));

  uint16_t copy_size = sizeof(int32_t);
  uint16_t stride = 1 + copy_size;  // Tight stride
  uint16_t offset = 0;              // Offset points to null flag
  uint32_t buffer_size = num_rows * stride;

  std::string bytecode_sequence = base::StackString<2048>(
                                      R"(
                                        AllocateRowLayoutBuffer: [buffer_size=%u, dest_buffer_register=Register(1)]
                                        PrefixPopcount: [col=0, dest_register=Register(2)]
                                        CopyToRowLayoutSparseNull: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(1), popcount_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
                                      )",
                                      buffer_size, offset, stride, copy_size)
                                      .ToStdString();

  std::vector<uint32_t> indices = {0, 1, 2, 3, 4};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));

  // Verification
  const auto& buffer = GetRegister<Slab<uint8_t>>(1);
  ASSERT_EQ(buffer.size(), buffer_size);

  struct ExpectedRow {
    bool non_null;
    int32_t value_if_non_null;
  };
  ExpectedRow expected_data[] = {
      {true, 10}, {false, 0}, {true, 30}, {false, 0}, {true, 50}};

  for (uint32_t i = 0; i < num_rows; ++i) {
    const uint8_t* row_start = buffer.data() + i * stride;
    uint8_t null_flag = row_start[offset];
    EXPECT_EQ(null_flag, static_cast<uint8_t>(expected_data[i].non_null))
        << "Null flag mismatch at row " << i;

    int32_t actual_value;
    memcpy(&actual_value, row_start + offset + 1, sizeof(int32_t));
    if (expected_data[i].non_null) {
      EXPECT_EQ(actual_value, expected_data[i].value_if_non_null)
          << "Data mismatch at row " << i;
    } else {
      std::vector<uint8_t> zeros(sizeof(int32_t), 0);
      EXPECT_EQ(memcmp(row_start + offset + 1, zeros.data(), sizeof(int32_t)),
                0)
          << "Null data not zeroed at row " << i;
    }
  }
}

TEST_F(BytecodeInterpreterTest, Distinct_TwoNonNullCols_SimpleDuplicates) {
  columns_vec_.push_back(
      CreateNonNullUnsortedColumn<int32_t>("col_int", {10, 20, 10, 30, 20}));
  columns_vec_.push_back(CreateNonNullUnsortedColumn<StringPool::Id>(
      "col_str", {"A", "B", "A", "C", "B"}, &spool_));

  uint16_t int_size = sizeof(int32_t);
  uint16_t str_id_size = sizeof(StringPool::Id);
  uint16_t stride = int_size + str_id_size;
  uint32_t num_rows = 5;
  uint32_t buffer_size = num_rows * stride;
  uint16_t col0_offset = 0;
  uint16_t col1_offset = int_size;

  std::string bytecode_sequence =
      base::StackString<2048>(
          R"(
            AllocateRowLayoutBuffer: [buffer_size=%u, dest_buffer_register=Register(2)]
            CopyToRowLayoutNonNull: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            CopyToRowLayoutNonNull: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            Distinct: [buffer_register=Register(2), total_row_stride=%u, indices_register=Register(0)]
          )",
          buffer_size, col0_offset, stride, int_size, col1_offset, stride,
          str_id_size, static_cast<uint32_t>(stride))
          .ToStdString();

  std::vector<uint32_t> indices = {0, 1, 2, 3, 4};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 1, 3));
}

TEST_F(BytecodeInterpreterTest,
       Distinct_TwoDenseNullCols_MixedNullsAndDuplicates) {
  uint32_t num_rows = 7;
  columns_vec_.push_back(CreateDenseNullableColumn<int32_t>(
      "col_int",
      {10, std::nullopt, 10, std::nullopt, 10, std::nullopt, std::nullopt}));
  columns_vec_.push_back(CreateDenseNullableStringColumn(
      "col_str",
      {std::nullopt, "B", "A", std::nullopt, std::nullopt, "B", std::nullopt},
      &spool_));

  uint16_t int_size = sizeof(int32_t);
  uint16_t str_id_size = sizeof(StringPool::Id);
  uint16_t stride = (1 + int_size) + (1 + str_id_size);
  uint32_t buffer_size = num_rows * stride;
  uint16_t col0_offset = 0;
  uint16_t col1_offset = 1 + int_size;

  std::string bytecode_sequence =
      base::StackString<2048>(
          R"(
            AllocateRowLayoutBuffer: [buffer_size=%u, dest_buffer_register=Register(2)]
            CopyToRowLayoutDenseNull: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            CopyToRowLayoutDenseNull: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            Distinct: [buffer_register=Register(2), total_row_stride=%u, indices_register=Register(0)]
          )",
          buffer_size, col0_offset, stride, int_size, col1_offset, stride,
          str_id_size, static_cast<uint32_t>(stride))
          .ToStdString();

  std::vector<uint32_t> indices(num_rows);
  std::iota(indices.begin(), indices.end(), 0);
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0),
              testing::UnorderedElementsAre(0, 1, 2, 3));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), SizeIs(4));
}

TEST_F(BytecodeInterpreterTest,
       Distinct_TwoSparseNullCols_MixedNullsAndDuplicates) {
  uint32_t num_rows = 7;
  columns_vec_.push_back(CreateSparseNullableColumn<int32_t>(
      "col_int",
      {10, std::nullopt, 10, std::nullopt, 10, std::nullopt, std::nullopt}));
  columns_vec_.push_back(CreateSparseNullableStringColumn(
      "col_str",
      {std::nullopt, "B", "A", std::nullopt, std::nullopt, "B", std::nullopt},
      &spool_));

  uint16_t int_size = sizeof(int32_t);
  uint16_t str_id_size = sizeof(StringPool::Id);
  uint16_t stride = (1 + int_size) + (1 + str_id_size);
  uint32_t buffer_size = num_rows * stride;
  uint16_t col0_offset = 0;
  uint16_t col1_offset = 1 + int_size;

  std::string bytecode_sequence =
      base::StackString<2048>(
          R"(
            AllocateRowLayoutBuffer: [buffer_size=%u, dest_buffer_register=Register(2)]
            PrefixPopcount: [col=0, dest_register=Register(3)]
            CopyToRowLayoutSparseNull: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), popcount_register=Register(3), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            PrefixPopcount: [col=1, dest_register=Register(4)]
            CopyToRowLayoutSparseNull: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), popcount_register=Register(4), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            Distinct: [buffer_register=Register(2), total_row_stride=%u, indices_register=Register(0)]
        )",
          buffer_size, col0_offset, stride, int_size, col1_offset, stride,
          str_id_size, static_cast<uint32_t>(stride))
          .ToStdString();

  std::vector<uint32_t> indices(num_rows);
  std::iota(indices.begin(), indices.end(), 0);
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0),
              testing::UnorderedElementsAre(0, 1, 2, 3));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), SizeIs(4));
}

TEST_F(BytecodeInterpreterTest, Distinct_TwoNonNullCols_InputAlreadyDistinct) {
  columns_vec_.push_back(
      CreateNonNullUnsortedColumn<int32_t>("col_int", {10, 20, 30}));
  columns_vec_.push_back(CreateNonNullUnsortedColumn<StringPool::Id>(
      "col_str", {"A", "B", "C"}, &spool_));

  uint16_t int_size = sizeof(int32_t);
  uint16_t str_id_size = sizeof(StringPool::Id);
  uint16_t stride = int_size + str_id_size;
  uint32_t num_rows = 3;
  uint32_t buffer_size = num_rows * stride;
  uint16_t col0_offset = 0;
  uint16_t col1_offset = int_size;

  std::string bytecode_sequence =
      base::StackString<2048>(
          R"(
            AllocateRowLayoutBuffer: [buffer_size=%u, dest_buffer_register=Register(2)]
            CopyToRowLayoutNonNull: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            CopyToRowLayoutNonNull: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            Distinct: [buffer_register=Register(2), total_row_stride=%u, indices_register=Register(0)]
          )",
          buffer_size, col0_offset, stride, int_size, col1_offset, stride,
          str_id_size, static_cast<uint32_t>(stride))
          .ToStdString();

  std::vector<uint32_t> indices = {0, 1, 2};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 1, 2));
}

TEST_F(BytecodeInterpreterTest, Distinct_EmptyInput) {
  columns_vec_.push_back(
      CreateNonNullUnsortedColumn<int32_t, int32_t>("col_int", {}));
  columns_vec_.push_back(
      CreateNonNullUnsortedColumn<StringPool::Id, const char*>("col_str", {},
                                                               &spool_));

  uint16_t int_size = sizeof(int32_t);
  uint16_t str_id_size = sizeof(StringPool::Id);
  uint16_t stride = int_size + str_id_size;
  uint32_t num_rows = 0;
  uint32_t buffer_size = num_rows * stride;
  uint16_t col0_offset = 0;
  uint16_t col1_offset = int_size;

  std::string bytecode_sequence =
      base::StackString<2048>(
          R"(
            AllocateRowLayoutBuffer: [buffer_size=%u, dest_buffer_register=Register(2)]
            CopyToRowLayoutNonNull: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            CopyToRowLayoutNonNull: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
            Distinct: [buffer_register=Register(2), total_row_stride=%u, indices_register=Register(0)]
          )",
          buffer_size, col0_offset, stride, int_size, col1_offset, stride,
          str_id_size, static_cast<uint32_t>(stride))
          .ToStdString();

  std::vector<uint32_t> indices = {};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), IsEmpty());
}

TEST_F(BytecodeInterpreterTest, Distinct_OneNonNullCol_SimpleDuplicates) {
  columns_vec_.push_back(
      CreateNonNullUnsortedColumn<int32_t>("col_int", {10, 20, 10, 30, 20}));

  uint16_t int_size = sizeof(int32_t);
  uint16_t stride = int_size;
  uint32_t num_rows = 5;
  uint32_t buffer_size = num_rows * stride;
  uint16_t col0_offset = 0;

  std::string bytecode_sequence = base::StackString<2048>(
                                      R"(
                                        AllocateRowLayoutBuffer: [buffer_size=%u, dest_buffer_register=Register(2)]
                                        CopyToRowLayoutNonNull: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=%u, row_layout_stride=%u, copy_size=%u]
                                        Distinct: [buffer_register=Register(2), total_row_stride=%u, indices_register=Register(0)]
                                      )",
                                      buffer_size, col0_offset, stride,
                                      int_size, static_cast<uint32_t>(stride))
                                      .ToStdString();

  std::vector<uint32_t> indices = {0, 1, 2, 3, 4};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 1, 3));
}

TEST_F(BytecodeInterpreterTest, LimitOffsetIndicesCombined) {
  std::vector<uint32_t> initial_indices(20);
  std::iota(initial_indices.begin(), initial_indices.end(), 0);

  // Apply offset=5, limit=10
  std::string bytecode =
      "LimitOffsetIndices: [offset_value=5, limit_value=10, "
      "update_register=Register(0)]";

  SetRegistersAndExecute(bytecode, GetSpan(initial_indices));

  // Expected result: Indices 5, 6, ..., 14 (size 10)
  const auto& result_span = GetRegister<Span<uint32_t>>(0);
  std::vector<uint32_t> expected_result(10);
  std::iota(expected_result.begin(), expected_result.end(), 5);
  EXPECT_THAT(result_span, ElementsAreArray(expected_result));
}

TEST_F(BytecodeInterpreterTest, LimitOffsetIndicesOffsetMakesEmpty) {
  std::vector<uint32_t> initial_indices(10);
  std::iota(initial_indices.begin(), initial_indices.end(), 0);

  std::string bytecode =
      "LimitOffsetIndices: [offset_value=10, limit_value=5, "
      "update_register=Register(0)]";

  SetRegistersAndExecute(bytecode, GetSpan(initial_indices));

  // Expected result: Empty span
  EXPECT_TRUE(GetRegister<Span<uint32_t>>(0).empty());

  // Test offset > size as well
  initial_indices.assign(10, 0);
  std::iota(initial_indices.begin(), initial_indices.end(), 0);
  bytecode =
      "LimitOffsetIndices: [offset_value=15, limit_value=5, "
      "update_register=Register(0)]";
  SetRegistersAndExecute(bytecode, GetSpan(initial_indices));
  EXPECT_TRUE(GetRegister<Span<uint32_t>>(0).empty());
}

TEST_F(BytecodeInterpreterTest, FindMinMaxIndexUint32) {
  columns_vec_.emplace_back(
      CreateNonNullUnsortedColumn<uint32_t>("col", {50u, 10u, 30u, 20u, 40u}));

  std::vector<uint32_t> initial_indices = {0, 1, 2, 3, 4};
  {
    std::vector<uint32_t> indices = initial_indices;
    std::string bytecode =
        "FindMinMaxIndex<Uint32, MinOp>: [col=0, update_register=Register(0)]";
    SetRegistersAndExecute(bytecode, GetSpan(indices));
    // Expected result: Span containing only index 1 (where value 10u is)
    EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(1u));
  }
  {
    // Use a fresh copy for testing MaxOp
    std::vector<uint32_t> indices = initial_indices;
    std::string bytecode =
        "FindMinMaxIndex<Uint32, MaxOp>: [col=0, update_register=Register(0)]";
    SetRegistersAndExecute(bytecode, GetSpan(indices));
    // Expected result: Span containing only index 0 (where value 50u is)
    EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0u));
  }
}

TEST_F(BytecodeInterpreterTest, FindMinMaxIndexString) {
  columns_vec_.emplace_back(CreateNonNullUnsortedColumn<StringPool::Id>(
      "col_str", {"banana", "apple", "cherry", "date", "apricot"}, &spool_));

  std::vector<uint32_t> initial_indices = {0, 1, 2, 3, 4};
  {
    std::vector<uint32_t> indices = initial_indices;
    std::string bytecode =
        "FindMinMaxIndex<String, MinOp>: [col=0, update_register=Register(0)]";
    SetRegistersAndExecute(bytecode, GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(1u));
  }
  {
    std::vector<uint32_t> indices = initial_indices;
    std::string bytecode =
        "FindMinMaxIndex<String, MaxOp>: [col=0, update_register=Register(0)]";
    SetRegistersAndExecute(bytecode, GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(3u));
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe::impl::bytecode
