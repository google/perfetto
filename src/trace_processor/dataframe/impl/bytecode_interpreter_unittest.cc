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
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/variant.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/impl/bit_vector.h"
#include "src/trace_processor/dataframe/impl/bytecode_core.h"
#include "src/trace_processor/dataframe/impl/bytecode_interpreter_impl.h"  // IWYU pragma: keep
#include "src/trace_processor/dataframe/impl/bytecode_interpreter_test_utils.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/flex_vector.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/types.h"
#include "src/trace_processor/util/regex.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe::impl::bytecode {
namespace {

using testing::AllOf;
using testing::ElementsAre;
using testing::ElementsAreArray;
using testing::IsEmpty;
using testing::Pointee;
using testing::SizeIs;
using testing::UnorderedElementsAre;

class BytecodeInterpreterTest : public testing::Test {
 protected:
  template <typename... Ts>
  void SetRegistersAndExecute(const std::string& bytecode_str, Ts... value) {
    SetupInterpreterWithBytecode(ParseBytecodeToVec(bytecode_str));
    SetRegisterValuesForTesting(
        interpreter_.get(),
        std::make_integer_sequence<uint32_t, sizeof...(Ts)>(),
        std::move(value)...);
    Execute();
  }

  // Intentionally not inlined to avoid inlining the entire
  // Interpreter::Execute() function, which is large and not needed for
  // testing purposes.
  PERFETTO_NO_INLINE void Execute() { interpreter_->Execute(fetcher_); }

  PERFETTO_NO_INLINE void SetupInterpreterWithBytecode(
      const BytecodeVector& bytecode) {
    // Hardcode the register count to 128 for testing.
    static constexpr uint32_t kNumRegisters = 128;
    interpreter_ = std::make_unique<Interpreter<Fetcher>>();
    interpreter_->Initialize(bytecode, kNumRegisters, column_ptrs_.data(),
                             indexes_.data(), &spool_);
  }

  template <typename T>
  const T& GetRegister(uint32_t reg_idx) {
    const auto* r = interpreter_->GetRegisterValue(reg::ReadHandle<T>(reg_idx));
    PERFETTO_CHECK(r);
    return *r;
  }

  void AddColumn(Column column) {
    columns_vec_.emplace_back(std::make_unique<Column>(std::move(column)));
    column_ptrs_.emplace_back(columns_vec_.back().get());
  }

  template <typename... Ts, uint32_t... Is>
  void SetRegisterValuesForTesting(Interpreter<Fetcher>* interpreter,
                                   std::integer_sequence<uint32_t, Is...>,
                                   Ts... values) {
    (interpreter->SetRegisterValueForTesting(reg::WriteHandle<Ts>(Is),
                                             std::move(values)),
     ...);
  }

  Fetcher fetcher_;
  StringPool spool_;
  std::vector<std::unique_ptr<Column>> columns_vec_;
  std::vector<Column*> column_ptrs_;
  std::vector<dataframe::Index> indexes_;
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

TEST_F(BytecodeInterpreterTest, Reverse) {
  std::vector<uint32_t> res = {1, 2, 3, 4, 5};
  SetRegistersAndExecute("Reverse: [update_register=Register(0)]",
                         GetSpan(res));

  const auto& update = GetRegister<Span<uint32_t>>(0);
  ASSERT_THAT(update.b, AllOf(testing::Ge(res.data()),
                              testing::Le(res.data() + res.size())));
  ASSERT_THAT(update.e, AllOf(testing::Ge(res.data()),
                              testing::Le(res.data() + res.size())));
  EXPECT_THAT(update, ElementsAre(5, 4, 3, 2, 1));
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
  fetcher_.value.push_back(input);
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
                     CastResult::NoneMatch(), dataframe::Eq{}},
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
        CastTestCase{"String", FilterValue{123.45}, CastResult::NoneMatch(),
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

TEST_F(BytecodeInterpreterTest, SortedFilter_LowerBound_BeginBound_Normal) {
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

TEST_F(BytecodeInterpreterTest, SortedFilter_LowerBound_EndBound_EmptiesRange) {
  std::string bytecode =
      "SortedFilter<Id, LowerBound>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(2)]";
  SetRegistersAndExecute(
      bytecode, CastFilterValueResult::Valid(CastFilterValueResult::Id{2}),
      Range{5, 10});

  const auto& result = GetRegister<Range>(1);
  EXPECT_EQ(result.b, 5u);
  EXPECT_EQ(result.e, 5u);
}

TEST_F(BytecodeInterpreterTest,
       SortedFilter_UpperBound_BeginBound_EmptiesRange) {
  std::string bytecode =
      "SortedFilter<Id, UpperBound>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(1)]";
  SetRegistersAndExecute(
      bytecode, CastFilterValueResult::Valid(CastFilterValueResult::Id{15}),
      Range{5, 10});

  const auto& result = GetRegister<Range>(1);
  EXPECT_EQ(result.b, 16u);
  EXPECT_EQ(result.e, 16u);
}

TEST_F(BytecodeInterpreterTest, SortedFilter_UpperBound_EndBound_Normal) {
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

TEST_F(BytecodeInterpreterTest, SortedFilter_UpperBound_EndBound_Redundant) {
  std::string bytecode =
      "SortedFilter<Id, UpperBound>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(2)]";
  SetRegistersAndExecute(
      bytecode, CastFilterValueResult::Valid(CastFilterValueResult::Id{12}),
      Range{0, 10});

  const auto& result = GetRegister<Range>(1);
  EXPECT_EQ(result.b, 0u);
  EXPECT_EQ(result.e, 10u);
}

TEST_F(BytecodeInterpreterTest, SortedFilterUint32Eq) {
  std::string bytecode =
      "SortedFilter<Uint32, EqualRange>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(0)]";

  auto values =
      CreateFlexVectorForTesting<uint32_t>({0u, 4u, 5u, 5u, 5u, 6u, 10u, 10u});
  AddColumn(impl::Column{std::move(values), NullStorage::NonNull{}, Sorted{},
                         HasDuplicates{}});
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
  AddColumn(impl::Column{Storage{std::move(values)},
                         NullStorage{NullStorage::NonNull{}}, Sorted{},
                         HasDuplicates{}});

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
  AddColumn(impl::Column{std::move(values), NullStorage::NonNull{}, Sorted{},
                         HasDuplicates{}});

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
  AddColumn(impl::Column{std::move(values), NullStorage::NonNull{}, Unsorted{},
                         HasDuplicates{}});

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
  AddColumn(impl::Column{std::move(values), NullStorage::NonNull{}, Sorted{},
                         HasDuplicates{}});

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
  AddColumn(impl::Column{std::move(values), NullStorage::NonNull{}, Unsorted{},
                         HasDuplicates{}});

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
  AddColumn(impl::Column{
      Storage{Storage::Uint32{}},  // Storage type doesn't matter for NullFilter
      NullStorage{NullStorage::DenseNull{std::move(bv)}}, Unsorted{},
      HasDuplicates{}});

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

  AddColumn(
      impl::Column{Storage{Storage::Uint32{}},  // Storage type doesn't matter
                   NullStorage{NullStorage::SparseNull{std::move(bv), {}}},
                   Unsorted{}, HasDuplicates{}});
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

  AddColumn(
      impl::Column{Storage{Storage::Uint32{}},  // Storage type doesn't matter
                   NullStorage{NullStorage::SparseNull{std::move(bv), {}}},
                   Unsorted{}, HasDuplicates{}});

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
  AddColumn(
      impl::Column{Storage{Storage::Uint32{}},  // Storage type doesn't matter
                   NullStorage{NullStorage::SparseNull{std::move(bv), {}}},
                   Unsorted{}, HasDuplicates{}});

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
  AddColumn(
      impl::Column{Storage{Storage::Uint32{}},  // Storage type doesn't matter
                   NullStorage{NullStorage::DenseNull{std::move(bv)}},
                   Unsorted{}, HasDuplicates{}});

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
  AddColumn(impl::Column{std::move(values), NullStorage{NullStorage::NonNull{}},
                         Unsorted{}, HasDuplicates{}});

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
  AddColumn(impl::Column{std::move(values), NullStorage{NullStorage::NonNull{}},
                         SetIdSorted{}, HasDuplicates{}});

  std::string bytecode =
      "Uint32SetIdSortedEq: [col=0, val_register=Register(0), "
      "update_register=Register(1)]";

  auto RunSubTest = [&](const std::string& label, Range initial_range,
                        uint32_t filter_val, Range expected_range) {
    SCOPED_TRACE("Sub-test: " + label);
    SetRegistersAndExecute(bytecode, CastFilterValueResult::Valid(filter_val),
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

TEST_F(BytecodeInterpreterTest, Distinct_TwoNonNullCols_SimpleDuplicates) {
  AddColumn(CreateNonNullColumn<int32_t, int32_t>({10, 20, 10, 30, 20},
                                                  Unsorted{}, HasDuplicates{}));
  AddColumn(CreateNonNullStringColumn<const char*>(
      {"A", "B", "A", "C", "B"}, Unsorted{}, HasDuplicates{}, &spool_));

  std::string bytecode_sequence = R"(
    AllocateRowLayoutBuffer: [buffer_size=40, dest_buffer_register=Register(2)]
    CopyToRowLayout<Int32, NonNull>: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=0, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    CopyToRowLayout<String, NonNull>: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=4, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    Distinct: [buffer_register=Register(2), total_row_stride=8, indices_register=Register(0)]
  )";

  std::vector<uint32_t> indices = {0, 1, 2, 3, 4};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 1, 3));
}

TEST_F(BytecodeInterpreterTest,
       Distinct_TwoDenseNullCols_MixedNullsAndDuplicates) {
  uint32_t num_rows = 7;
  AddColumn(CreateDenseNullableColumn<int32_t>(
      {10, std::nullopt, 10, std::nullopt, 10, std::nullopt, std::nullopt},
      Unsorted{}, HasDuplicates{}));
  AddColumn(CreateDenseNullableStringColumn(
      {std::nullopt, "B", "A", std::nullopt, std::nullopt, "B", std::nullopt},
      &spool_, Unsorted{}, HasDuplicates{}));

  std::string bytecode_sequence = R"(
    AllocateRowLayoutBuffer: [buffer_size=70, dest_buffer_register=Register(2)]
    CopyToRowLayout<Int32, DenseNull>: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=0, row_layout_stride=10, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    CopyToRowLayout<String, DenseNull>: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=5, row_layout_stride=10, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    Distinct: [buffer_register=Register(2), total_row_stride=10, indices_register=Register(0)]
  )";

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
  AddColumn(CreateSparseNullableColumn<int32_t>(
      {10, std::nullopt, 10, std::nullopt, 10, std::nullopt, std::nullopt},
      Unsorted{}, HasDuplicates{}));
  AddColumn(CreateSparseNullableStringColumn(
      {std::nullopt, "B", "A", std::nullopt, std::nullopt, "B", std::nullopt},
      &spool_, Unsorted{}, HasDuplicates{}));

  std::string bytecode_sequence = R"(
    AllocateRowLayoutBuffer: [buffer_size=70, dest_buffer_register=Register(2)]
    PrefixPopcount: [col=0, dest_register=Register(3)]
    PrefixPopcount: [col=1, dest_register=Register(4)]
    CopyToRowLayout<Int32, SparseNull>: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=0, row_layout_stride=10, invert_copied_bits=0, popcount_register=Register(3), rank_map_register=Register(4294967295)]
    CopyToRowLayout<String, SparseNull>: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=5, row_layout_stride=10, invert_copied_bits=0, popcount_register=Register(4), rank_map_register=Register(4294967295)]
    Distinct: [buffer_register=Register(2), total_row_stride=10, indices_register=Register(0)]
  )";

  std::vector<uint32_t> indices(num_rows);
  std::iota(indices.begin(), indices.end(), 0);
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0),
              testing::UnorderedElementsAre(0, 1, 2, 3));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), SizeIs(4));
}

TEST_F(BytecodeInterpreterTest, Distinct_TwoNonNullCols_InputAlreadyDistinct) {
  AddColumn(CreateNonNullColumn<int32_t, int32_t>({10, 20, 30}, Unsorted{},
                                                  HasDuplicates{}));
  AddColumn(CreateNonNullStringColumn<const char*>({"A", "B", "C"}, Unsorted{},
                                                   HasDuplicates{}, &spool_));

  std::string bytecode_sequence = R"(
    AllocateRowLayoutBuffer: [buffer_size=24, dest_buffer_register=Register(2)]
    CopyToRowLayout<Int32, NonNull>: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=0, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    CopyToRowLayout<String, NonNull>: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=4, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    Distinct: [buffer_register=Register(2), total_row_stride=8, indices_register=Register(0)]
  )";

  std::vector<uint32_t> indices = {0, 1, 2};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 1, 2));
}

TEST_F(BytecodeInterpreterTest, Distinct_EmptyInput) {
  AddColumn(
      CreateNonNullColumn<int32_t, int32_t>({}, Unsorted{}, HasDuplicates{}));
  AddColumn(CreateNonNullStringColumn<const char*>({}, Unsorted{},
                                                   HasDuplicates{}, &spool_));

  std::string bytecode_sequence = R"(
    AllocateRowLayoutBuffer: [buffer_size=0, dest_buffer_register=Register(2)]
    CopyToRowLayout<Int32, NonNull>: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=0, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    CopyToRowLayout<String, NonNull>: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=4, row_layout_stride=8, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    Distinct: [buffer_register=Register(2), total_row_stride=8, indices_register=Register(0)]
  )";

  std::vector<uint32_t> indices = {};
  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), IsEmpty());
}

TEST_F(BytecodeInterpreterTest, Distinct_OneNonNullCol_SimpleDuplicates) {
  AddColumn(CreateNonNullColumn<int32_t, int32_t>({10, 20, 10, 30, 20},
                                                  Unsorted{}, HasDuplicates{}));

  std::string bytecode_sequence = R"(
    AllocateRowLayoutBuffer: [buffer_size=20, dest_buffer_register=Register(2)]
    CopyToRowLayout<Int32, NonNull>: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(2), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    Distinct: [buffer_register=Register(2), total_row_stride=4, indices_register=Register(0)]
  )";

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
  AddColumn(CreateNonNullColumn<uint32_t, uint32_t>(
      {50u, 10u, 30u, 20u, 40u}, Unsorted{}, HasDuplicates{}));

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
  AddColumn(CreateNonNullStringColumn<const char*>(
      {"banana", "apple", "cherry", "date", "apricot"}, Unsorted{},
      HasDuplicates{}, &spool_));

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

TEST_F(BytecodeInterpreterTest, IndexPermutationVectorToSpan) {
  std::vector<uint32_t> p_vec = {2, 0, 4, 1, 3};
  auto shared_p_vec = std::make_shared<std::vector<uint32_t>>(p_vec);
  indexes_.emplace_back(std::vector<uint32_t>{0}, shared_p_vec);

  std::string bytecode_str =
      "IndexPermutationVectorToSpan: [index=0, write_register=Register(0)]";
  SetRegistersAndExecute(bytecode_str);
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), testing::ElementsAreArray(p_vec));
}

TEST_F(BytecodeInterpreterTest, IndexPermutationVectorToSpan_Empty) {
  std::vector<uint32_t> p_vec = {};
  auto shared_p_vec = std::make_shared<std::vector<uint32_t>>(p_vec);
  indexes_.emplace_back(std::vector<uint32_t>{0}, shared_p_vec);

  std::string bytecode_str =
      "IndexPermutationVectorToSpan: [index=0, write_register=Register(0)]";
  SetRegistersAndExecute(bytecode_str);
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), IsEmpty());
}

TEST_F(BytecodeInterpreterTest, IndexedFilterEq_Uint32_NonNull_ValueExists) {
  AddColumn(CreateNonNullColumn<uint32_t, uint32_t>(
      {10u, 20u, 20u, 30u, 20u, 40u}, Unsorted{}, HasDuplicates{}));

  std::vector<uint32_t> p_vec = {0, 1, 4, 2, 3, 5};
  indexes_.emplace_back(std::vector<uint32_t>{0},
                        std::make_shared<std::vector<uint32_t>>(p_vec));

  std::string bytecode_str = R"(
    IndexedFilterEq<Uint32, NonNull>: [col=0, filter_value_reg=Register(0), popcount_register=Register(1), update_register=Register(2)]
  )";
  SetRegistersAndExecute(bytecode_str, CastFilterValueResult::Valid(20u),
                         Slab<uint32_t>::Alloc(0), GetSpan(p_vec));

  EXPECT_THAT(GetRegister<Span<uint32_t>>(2), testing::ElementsAre(1, 4, 2));
}

TEST_F(BytecodeInterpreterTest, IndexedFilterEq_Uint32_NonNull_ValueNotExists) {
  AddColumn(CreateNonNullColumn<uint32_t, uint32_t>(
      {10u, 20u, 20u, 30u, 20u, 40u}, Unsorted{}, HasDuplicates{}));
  std::vector<uint32_t> p_vec = {0, 1, 4, 2, 3, 5};
  indexes_.emplace_back(std::vector<uint32_t>{0},
                        std::make_shared<std::vector<uint32_t>>(p_vec));

  std::string bytecode_str = R"(
    IndexedFilterEq<Uint32, NonNull>: [col=0, filter_value_reg=Register(0), popcount_register=Register(1), update_register=Register(2)]
  )";
  SetRegistersAndExecute(bytecode_str, CastFilterValueResult::Valid(25u),
                         Slab<uint32_t>::Alloc(0), GetSpan(p_vec));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
}

TEST_F(BytecodeInterpreterTest, IndexedFilterEq_String_SparseNull_ValueExists) {
  AddColumn(CreateSparseNullableStringColumn(
      {std::make_optional("apple"), std::nullopt, std::make_optional("banana"),
       std::make_optional("apple"), std::nullopt},
      &spool_, Unsorted{}, HasDuplicates{}));

  std::vector<uint32_t> p_vec = {1, 4, 0, 3, 2};
  indexes_.emplace_back(std::vector<uint32_t>{0},
                        std::make_shared<std::vector<uint32_t>>(p_vec));

  std::string bytecode_str = R"(
    PrefixPopcount: [col=0, dest_register=Register(1)]
    IndexedFilterEq<String, SparseNull>: [col=0, filter_value_reg=Register(0), popcount_register=Register(1), update_register=Register(2)]
  )";
  SetRegistersAndExecute(bytecode_str, CastFilterValueResult::Valid("apple"),
                         reg::Empty(), GetSpan(p_vec));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(2), testing::ElementsAre(0, 3));
}

TEST_F(BytecodeInterpreterTest,
       IndexedFilterEq_String_SparseNull_ValueNotExists) {
  AddColumn(CreateSparseNullableStringColumn(
      {std::make_optional("cat"), std::nullopt, std::make_optional("dog")},
      &spool_, Unsorted{}, HasDuplicates{}));

  std::vector<uint32_t> p_vec = {1, 0, 2};
  indexes_.emplace_back(std::vector<uint32_t>{0},
                        std::make_shared<std::vector<uint32_t>>(p_vec));

  std::string bytecode_str = R"(
    PrefixPopcount: [col=0, dest_register=Register(1)]
    IndexedFilterEq<String, SparseNull>: [col=0, filter_value_reg=Register(0), popcount_register=Register(1), update_register=Register(2)]
  )";
  SetRegistersAndExecute(bytecode_str, CastFilterValueResult::Valid("bird"),
                         reg::Empty(), GetSpan(p_vec));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
}

TEST_F(BytecodeInterpreterTest, CopySpanIntersectingRange_PartialOverlap) {
  std::vector<uint32_t> source_span_data = {10, 20, 30, 40, 50};
  std::vector<uint32_t> update_buffer(source_span_data.size());

  std::string bytecode_str = R"(
    CopySpanIntersectingRange: [source_register=Register(0), source_range_register=Register(1), update_register=Register(2)]
  )";
  SetRegistersAndExecute(bytecode_str, GetSpan(source_span_data), Range{25, 45},
                         GetSpan(update_buffer));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(2), testing::ElementsAre(30, 40));
}

TEST_F(BytecodeInterpreterTest, CopySpanIntersectingRange_NoOverlap) {
  std::vector<uint32_t> source_span_data = {10, 20, 30};
  std::vector<uint32_t> update_buffer(source_span_data.size());

  std::string bytecode_str = R"(
    CopySpanIntersectingRange: [source_register=Register(0), source_range_register=Register(1), update_register=Register(2)]
  )";
  SetRegistersAndExecute(bytecode_str, GetSpan(source_span_data),
                         Range{100, 200}, GetSpan(update_buffer));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
}

TEST_F(BytecodeInterpreterTest, LinearFilterEq_Uint32_NonNull_ValueExists) {
  AddColumn(CreateNonNullColumn<uint32_t, uint32_t>(
      {10u, 20u, 20u, 30u, 20u, 40u}, Unsorted{}, HasDuplicates{}));

  std::string bytecode_str = R"(
    LinearFilterEq<Uint32>: [col=0, filter_value_reg=Register(0), popcount_register=Register(1), source_register=Register(2), update_register=Register(3)]
  )";
  // Initial range covers all elements.
  Range source_range{0, 6};
  std::vector<uint32_t> update_data(
      6);  // Sufficient space for all possible matches

  SetRegistersAndExecute(
      bytecode_str, CastFilterValueResult::Valid(20u),
      Slab<uint32_t>::Alloc(0),  // Dummy popcount for NonNull
      source_range, GetSpan(update_data));

  // Expected indices where data[i] == 20u: 1, 2, 4
  EXPECT_THAT(GetRegister<Span<uint32_t>>(3), ElementsAre(1u, 2u, 4u));
}

TEST_F(BytecodeInterpreterTest, LinearFilterEq_Uint32_NonNull_ValueNotExists) {
  AddColumn(CreateNonNullColumn<uint32_t, uint32_t>({10u, 20u, 30u}, Unsorted{},
                                                    HasDuplicates{}));

  std::string bytecode_str = R"(
    LinearFilterEq<Uint32>: [col=0, filter_value_reg=Register(0), popcount_register=Register(1), source_register=Register(2), update_register=Register(3)]
  )";
  Range source_range{0, 3};
  std::vector<uint32_t> update_data(3);

  SetRegistersAndExecute(bytecode_str, CastFilterValueResult::Valid(25u),
                         Slab<uint32_t>::Alloc(0),  // Dummy popcount
                         source_range, GetSpan(update_data));

  EXPECT_THAT(GetRegister<Span<uint32_t>>(3), IsEmpty());
}

TEST_F(BytecodeInterpreterTest, LinearFilterEq_String_NonNull_ValueExists) {
  AddColumn(CreateNonNullStringColumn<const char*>(
      {"apple", "banana", "apple", "cherry"}, Unsorted{}, HasDuplicates{},
      &spool_));

  std::string bytecode_str = R"(
    LinearFilterEq<String>: [col=0, filter_value_reg=Register(0), popcount_register=Register(1), source_register=Register(2), update_register=Register(3)]
  )";
  Range source_range{0, 4};
  std::vector<uint32_t> update_data(4);

  SetRegistersAndExecute(bytecode_str, CastFilterValueResult::Valid("apple"),
                         Slab<uint32_t>::Alloc(0),  // Dummy popcount
                         source_range, GetSpan(update_data));

  // Expected indices where data[i] == "apple": 0, 2
  EXPECT_THAT(GetRegister<Span<uint32_t>>(3), ElementsAre(0u, 2u));
}

TEST_F(BytecodeInterpreterTest, LinearFilterEq_HandleInvalidCast_NoneMatch) {
  AddColumn(CreateNonNullColumn<uint32_t, uint32_t>({10u, 20u, 30u}, Unsorted{},
                                                    HasDuplicates{}));
  std::string bytecode_str = R"(
    LinearFilterEq<Uint32>: [col=0, filter_value_reg=Register(0), popcount_register=Register(1), source_register=Register(2), update_register=Register(3)]
  )";
  Range source_range{0, 3};
  std::vector<uint32_t> update_data(3);

  // Intentionally not pre-filling update_data to ensure iota in LinearFilterEq
  // (user version) correctly handles an empty effective range.
  SetRegistersAndExecute(bytecode_str, CastFilterValueResult::NoneMatch(),
                         Slab<uint32_t>::Alloc(0), source_range,
                         GetSpan(update_data));

  // HandleInvalidCastFilterValueResult should make the source_range empty,
  // then the iota in the user's corrected LinearFilterEq will copy 0 elements.
  EXPECT_THAT(GetRegister<Span<uint32_t>>(3), IsEmpty());
}

TEST_F(BytecodeInterpreterTest, LinearFilterEq_HandleInvalidCast_AllMatch) {
  AddColumn(CreateNonNullColumn<uint32_t, uint32_t>({10u, 20u, 30u}, Unsorted{},
                                                    HasDuplicates{}));
  std::string bytecode_str = R"(
    LinearFilterEq<Uint32>: [col=0, filter_value_reg=Register(0), popcount_register=Register(1), source_register=Register(2), update_register=Register(3)]
  )";
  Range source_range{0, 3};
  std::vector<uint32_t> update_data(3);

  SetRegistersAndExecute(bytecode_str, CastFilterValueResult::AllMatch(),
                         Slab<uint32_t>::Alloc(0), source_range,
                         GetSpan(update_data));
  // HandleInvalidCastFilterValueResult returns early, source_range is not
  // modified. The iota in LinearFilterEq (user corrected version) copies all
  // original indices from the range.
  EXPECT_THAT(GetRegister<Span<uint32_t>>(3), ElementsAre(0u, 1u, 2u));
}

TEST_F(BytecodeInterpreterTest, CollectIdIntoRankMap) {
  AddColumn(CreateSparseNullableStringColumn(
      {std::make_optional("apple"), std::nullopt, std::make_optional("banana")},
      &spool_, Unsorted{}, HasDuplicates{}));

  std::vector<uint32_t> data = {0, 1};

  std::string bytecode_str = R"(
    InitRankMap: [dest_register=Register(1)]
    CollectIdIntoRankMap: [col=0, source_register=Register(0), rank_map_register=Register(1)]
  )";
  SetRegistersAndExecute(
      bytecode_str, Span<uint32_t>(data.data(), data.data() + data.size()));

  const auto& rank_map = *GetRegister<reg::StringIdToRankMap>(1);
  EXPECT_EQ(rank_map.size(), 2u);
  EXPECT_THAT(rank_map.Find(*spool_.GetId("apple")), Pointee(0u));
  EXPECT_THAT(rank_map.Find(*spool_.GetId("banana")), Pointee(0u));
}

TEST_F(BytecodeInterpreterTest, FinalizeRanksInMap_Simple) {
  StringPool::Id apple_id = spool_.InternString("apple");
  StringPool::Id banana_id = spool_.InternString("banana");
  StringPool::Id cherry_id = spool_.InternString("cherry");

  auto map = std::make_unique<base::FlatHashMap<StringPool::Id, uint32_t>>();
  map->Insert(banana_id, 0);
  map->Insert(cherry_id, 0);
  map->Insert(apple_id, 0);

  std::string bytecode_str =
      "FinalizeRanksInMap: [update_register=Register(0)]";
  SetRegistersAndExecute(bytecode_str, std::move(map));

  const auto& rank_map = *GetRegister<reg::StringIdToRankMap>(0);
  EXPECT_EQ(rank_map.size(), 3u);
  EXPECT_THAT(rank_map.Find(apple_id), Pointee(0u));
  EXPECT_THAT(rank_map.Find(banana_id), Pointee(1u));
  EXPECT_THAT(rank_map.Find(cherry_id), Pointee(2u));
}

TEST_F(BytecodeInterpreterTest, Sort_SingleUint32Column_Ascending) {
  // Data: {30, 10, 40, 20}
  // Expected sorted indices: {1 (10), 3 (20), 0 (30), 2 (40)}
  uint32_t num_rows = 4;
  AddColumn(CreateNonNullColumn<uint32_t, uint32_t>(
      {30u, 10u, 40u, 20u}, Unsorted{}, HasDuplicates{}));  // col 0

  // Bytecode sequence:
  // 1. AllocateRowLayoutBuffer (stride = sizeof(uint32_t) = 4, size = 4*4 = 16)
  // 2. CopyToRowLayout<Uint32, NonNull> (invert_copied_bits = 0 for asc)
  // 3. SortRowLayout
  std::string bytecode_sequence = R"(
    AllocateRowLayoutBuffer: [buffer_size=16, dest_buffer_register=Register(1)]
    CopyToRowLayout<Uint32, NonNull>: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(1), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(1), total_row_stride=4, indices_register=Register(0)]
  )";

  std::vector<uint32_t> indices(num_rows);
  std::iota(indices.begin(), indices.end(), 0);  // {0, 1, 2, 3}

  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(1, 3, 0, 2));
}

TEST_F(BytecodeInterpreterTest,
       Sort_SingleStringColumn_Descending_WithRankMap) {
  // Data: {"cherry", "apple", "banana"}
  // Ranks (asc): apple (0), banana (1), cherry (2)
  // Expected sorted indices (desc): {0 (cherry), 2 (banana), 1 (apple)}
  uint32_t num_rows = 3;
  AddColumn(CreateNonNullStringColumn<const char*>(  // col 0
      {"cherry", "apple", "banana"}, Unsorted{}, HasDuplicates{}, &spool_));

  // Bytecode sequence:
  // 1. InitRankMap
  // 2. CollectIdIntoRankMap
  // 3. FinalizeRanksInMap
  // 4. AllocateRowLayoutBuffer (stride = sizeof(uint32_t) for rank = 4, size =
  // 3*4 = 12)
  // 5. CopyToRowLayout<String, NonNull> (invert_copied_bits = 1 for desc)
  // 6. SortRowLayout
  std::string bytecode_sequence = R"(
    InitRankMap: [dest_register=Register(2)]
    CollectIdIntoRankMap: [col=0, source_register=Register(0), rank_map_register=Register(2)]
    FinalizeRanksInMap: [update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=12, dest_buffer_register=Register(1)]
    CopyToRowLayout<String, NonNull>: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(1), row_layout_offset=0, row_layout_stride=4, invert_copied_bits=1, popcount_register=Register(4294967295), rank_map_register=Register(2)]
    SortRowLayout: [buffer_register=Register(1), total_row_stride=4, indices_register=Register(0)]
  )";

  std::vector<uint32_t> indices(num_rows);
  std::iota(indices.begin(), indices.end(), 0);  // {0, 1, 2}

  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(0, 2, 1));
}

TEST_F(BytecodeInterpreterTest,
       Sort_MultiColumn_Int64Desc_StringAsc_NullableInt32Asc) {
  // Data:
  // Row | Col 0 (Int64) | Col 1 (String) | Col 2 (Int32, SparseNull)
  // --- | ------------- | -------------- | -------------------------
  // 0   | 100           | "B"            | null
  // 1   | 200           | "A"            | 5
  // 2   | 100           | "A"            | 15
  // 3   | 200           | "C"            | null
  //
  // Sort Order:
  // 1. Col 0 (Int64) DESC
  // 2. Col 1 (String) ASC
  // 3. Col 2 (Int32, SparseNull) ASC (nulls first for ASC sort on nullable)
  //
  // Expected sorted indices:
  // Original: (200,A,5), (200,C,null), (100,A,15), (100,B,null)
  // Indices:  {1, 3, 2, 0}

  uint32_t num_rows = 4;
  AddColumn(CreateNonNullColumn<int64_t, int64_t>(
      {100, 200, 100, 200}, Unsorted{}, HasDuplicates{}));  // col 0
  AddColumn(CreateNonNullStringColumn<const char*>(         // col 1
      {"B", "A", "A", "C"}, Unsorted{}, HasDuplicates{}, &spool_));
  AddColumn(CreateSparseNullableColumn<int32_t>(  // col 2
      {std::nullopt, 5, 15, std::nullopt}, Unsorted{}, HasDuplicates{}));

  // Strides:
  // Col 0 (Int64): sizeof(int64_t) = 8
  // Col 1 (String rank): sizeof(uint32_t) = 4
  // Col 2 (Int32 SparseNull): 1 (null flag) + sizeof(int32_t) (4) = 5
  // Total row stride = 8 + 4 + 5 = 17
  // Buffer size = num_rows * total_row_stride = 4 * 17 = 68

  std::string bytecode_sequence = R"(
    PrefixPopcount: [col=2, dest_register=Register(3)]
    InitRankMap: [dest_register=Register(2)]
    CollectIdIntoRankMap: [col=1, source_register=Register(0), rank_map_register=Register(2)]
    FinalizeRanksInMap: [update_register=Register(2)]
    AllocateRowLayoutBuffer: [buffer_size=68, dest_buffer_register=Register(1)]
    CopyToRowLayout<Int64, NonNull>: [col=0, source_indices_register=Register(0), dest_buffer_register=Register(1), row_layout_offset=0, row_layout_stride=17, invert_copied_bits=1, popcount_register=Register(4294967295), rank_map_register=Register(4294967295)]
    CopyToRowLayout<String, NonNull>: [col=1, source_indices_register=Register(0), dest_buffer_register=Register(1), row_layout_offset=8, row_layout_stride=17, invert_copied_bits=0, popcount_register=Register(4294967295), rank_map_register=Register(2)]
    CopyToRowLayout<Int32, SparseNull>: [col=2, source_indices_register=Register(0), dest_buffer_register=Register(1), row_layout_offset=12, row_layout_stride=17, invert_copied_bits=0, popcount_register=Register(3), rank_map_register=Register(4294967295)]
    SortRowLayout: [buffer_register=Register(1), total_row_stride=17, indices_register=Register(0)]
  )";

  std::vector<uint32_t> indices(num_rows);
  std::iota(indices.begin(), indices.end(), 0);  // {0, 1, 2, 3}

  SetRegistersAndExecute(bytecode_sequence, GetSpan(indices));
  EXPECT_THAT(GetRegister<Span<uint32_t>>(0), ElementsAre(1, 3, 2, 0));
}

TEST_F(BytecodeInterpreterTest, InId) {
  AddColumn(impl::Column{impl::Storage::Id{}, impl::NullStorage::NonNull{},
                         Unsorted{}, HasDuplicates{}});
  std::string bytecode =
      "In<Id>: [col=0, value_list_register=Register(0), "
      "source_register=Register(1), update_register=Register(2)]";

  std::vector<uint32_t> indices_spec = {12, 44, 10, 4, 5, 2, 3};
  {
    // Test case 1: Values exist in range. This should trigger the bitvector
    // optimization as max(5, 10, 44) <= 3 * 16.
    std::vector<uint32_t> indices = indices_spec;
    CastFilterValueListResult value_list;
    value_list.validity = CastFilterValueResult::kValid;
    value_list.value_list =
        CreateFlexVectorForTesting<CastFilterValueResult::Id>(
            {{5}, {10}, {44}});

    SetRegistersAndExecute(bytecode, std::move(value_list), GetSpan(indices),
                           GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), ElementsAre(44, 10, 5));
  }
  {
    // Test case 2: No values exist in range
    std::vector<uint32_t> indices = indices_spec;
    CastFilterValueListResult value_list;
    value_list.validity = CastFilterValueResult::kValid;
    value_list.value_list =
        CreateFlexVectorForTesting<CastFilterValueResult::Id>({{100}, {200}});
    SetRegistersAndExecute(bytecode, std::move(value_list), GetSpan(indices),
                           GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
  }
  {
    // Test case 3: Invalid cast result (NoneMatch)
    std::vector<uint32_t> indices = indices_spec;
    CastFilterValueListResult value_list;
    value_list.validity = CastFilterValueResult::kNoneMatch;
    SetRegistersAndExecute(bytecode, std::move(value_list), GetSpan(indices),
                           GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
  }
}

TEST_F(BytecodeInterpreterTest, InUint32) {
  std::string bytecode =
      "In<Uint32>: [col=0, value_list_register=Register(0), "
      "source_register=Register(1), update_register=Register(2)]";

  auto values =
      CreateFlexVectorForTesting<uint32_t>({4u, 49u, 392u, 4u, 49u, 4u, 391u});
  AddColumn(impl::Column{std::move(values), NullStorage::NonNull{}, Unsorted{},
                         HasDuplicates{}});

  std::vector<uint32_t> indices_spec = {3, 3, 4, 5, 0, 6, 0};
  {
    // Test case 1: Values exist. This should not trigger the bitvector
    // optimization as max(4, 391) > 2 * 16.
    std::vector<uint32_t> indices = indices_spec;
    CastFilterValueListResult value_list;
    value_list.validity = CastFilterValueResult::kValid;
    value_list.value_list = CreateFlexVectorForTesting<uint32_t>({4u, 391u});

    SetRegistersAndExecute(bytecode, std::move(value_list), GetSpan(indices),
                           GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), ElementsAre(3, 3, 5, 0, 6, 0));
  }
  {
    // Test case 2: No values exist
    std::vector<uint32_t> indices = indices_spec;
    CastFilterValueListResult value_list;
    value_list.validity = CastFilterValueResult::kValid;
    value_list.value_list = CreateFlexVectorForTesting<uint32_t>({100u, 200u});
    SetRegistersAndExecute(bytecode, std::move(value_list), GetSpan(indices),
                           GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), IsEmpty());
  }
}

TEST_F(BytecodeInterpreterTest, InIdBitVectorSparse) {
  AddColumn(impl::Column{impl::Storage::Id{1000}, NullStorage::NonNull{},
                         Unsorted{}, HasDuplicates{}});

  std::string bytecode =
      "In<Id>: [col=0, value_list_register=Register(0), "
      "source_register=Register(1), update_register=Register(2)]";

  std::vector<uint32_t> indices_spec = {12, 44, 10, 4, 5, 2, 3, 500};
  {
    // Test case: Sparse values, bitvector optimization should NOT trigger.
    // max value is 500, list size is 2. 500 > 2 * 16 (32) is true.
    std::vector<uint32_t> indices = indices_spec;
    CastFilterValueListResult value_list;
    value_list.validity = CastFilterValueResult::kValid;
    value_list.value_list =
        CreateFlexVectorForTesting<CastFilterValueResult::Id>({{5}, {500}});

    SetRegistersAndExecute(bytecode, std::move(value_list), GetSpan(indices),
                           GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), ElementsAre(5, 500));
  }
}

TEST_F(BytecodeInterpreterTest, InUint32BitVector) {
  std::string bytecode =
      "In<Uint32>: [col=0, value_list_register=Register(0), "
      "source_register=Register(1), update_register=Register(2)]";

  auto values =
      CreateFlexVectorForTesting<uint32_t>({4u, 49u, 392u, 4u, 49u, 4u, 391u});
  AddColumn(impl::Column{std::move(values), NullStorage::NonNull{}, Unsorted{},
                         HasDuplicates{}});

  std::vector<uint32_t> indices_spec = {3, 3, 4, 5, 0, 6, 0};
  {
    // Test case: Values exist, bitvector optimization should trigger.
    // max value 30, list size 2. 30 <= 32 is true.
    std::vector<uint32_t> indices = indices_spec;
    CastFilterValueListResult value_list;
    value_list.validity = CastFilterValueResult::kValid;
    value_list.value_list = CreateFlexVectorForTesting<uint32_t>({4u, 30u});

    SetRegistersAndExecute(bytecode, std::move(value_list), GetSpan(indices),
                           GetSpan(indices));
    EXPECT_THAT(GetRegister<Span<uint32_t>>(2), ElementsAre(3, 3, 5, 0, 0));
  }
}

TEST_F(BytecodeInterpreterTest, CastFilterValueList_Uint32) {
  fetcher_.value.emplace_back(int64_t(10));
  fetcher_.value.emplace_back(int64_t(20));
  fetcher_.value.emplace_back(int64_t(-1));
  fetcher_.value.emplace_back(int64_t(std::numeric_limits<uint32_t>::max()) +
                              1);

  SetRegistersAndExecute(
      "CastFilterValueList<Uint32>: [fval_handle=FilterValue(0), "
      "write_register=Register(0), op=Op(0)]"  // Op(0) is Eq
  );

  const auto& result = GetRegister<CastFilterValueListResult>(0);
  ASSERT_EQ(result.validity, CastFilterValueResult::kValid);
  const auto& list = std::get<FlexVector<uint32_t>>(result.value_list);
  EXPECT_THAT(list, ElementsAre(10u, 20u));
}

TEST_F(BytecodeInterpreterTest, CastFilterValueList_String) {
  fetcher_.value.emplace_back("hello");
  fetcher_.value.emplace_back("world");
  fetcher_.value.emplace_back(int64_t(10));

  spool_.InternString("hello");
  spool_.InternString("world");

  SetRegistersAndExecute(
      "CastFilterValueList<String>: [fval_handle=FilterValue(0), "
      "write_register=Register(0), op=Op(0)]"  // Op(0) is Eq
  );

  const auto& result = GetRegister<CastFilterValueListResult>(0);
  ASSERT_EQ(result.validity, CastFilterValueResult::kValid);
  const auto& list = std::get<FlexVector<StringPool::Id>>(result.value_list);
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(spool_.Get(list[0]), "hello");
  EXPECT_EQ(spool_.Get(list[1]), "world");
}

TEST_F(BytecodeInterpreterTest, SortedFilterUint32Eq_ManyDuplicates) {
  std::string bytecode =
      "SortedFilter<Uint32, EqualRange>: [col=0, val_register=Register(0), "
      "update_register=Register(1), write_result_to=BoundModifier(0)]";

  auto values = CreateFlexVectorForTesting<uint32_t>(
      {0u, 4u, 5u, 5u, 5u, 5u, 5u, 5u, 5u, 5u, 5u,  5u, 5u,
       5u, 5u, 5u, 5u, 5u, 5u, 5u, 5u, 5u, 6u, 10u, 10u});
  AddColumn(impl::Column{std::move(values), NullStorage::NonNull{}, Sorted{},
                         HasDuplicates{}});

  SetRegistersAndExecute(bytecode, CastFilterValueResult::Valid(5u),
                         Range{0u, 25u});
  const auto& result = GetRegister<Range>(1);
  EXPECT_EQ(result.b, 2u);
  EXPECT_EQ(result.e, 22u);
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe::impl::bytecode
