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

#include "src/trace_processor/core/exec/assert_type.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

const char* Name(Variant::Type type) {
  switch (type) {
    case Variant::Type::kNull:
      return "a null";
    case Variant::Type::kInt64:
      return "an integer";
    case Variant::Type::kDouble:
      return "a float";
    case Variant::Type::kString:
      return "a string";
  }
  return "something";
}

const char* Name(StorageType type) {
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
    case StorageType::GetTypeIndex<Uint32>():
    case StorageType::GetTypeIndex<Int32>():
    case StorageType::GetTypeIndex<Int64>():
      return "an integer";
    case StorageType::GetTypeIndex<Double>():
      return "a float";
    case StorageType::GetTypeIndex<String>():
      return "a string";
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

base::Status NotExact(const std::string& name) {
  return base::ErrStatus(
      "column '%s' holds an integer which a float cannot represent exactly",
      name.c_str());
}

template <typename From, typename To>
bool Cast(From value, To* out) {
  *out = static_cast<To>(value);
  return true;
}

// Fails when no double holds exactly `value`: either it lies outside the
// range of a double or, above 2^53, its low bits would be rounded away.
bool ExactDouble(int64_t value, double* out) {
  constexpr double kInt64Limit = 0x1p63;
  double widened = static_cast<double>(value);
  if (widened >= kInt64Limit || widened < -kInt64Limit ||
      static_cast<int64_t>(widened) != value) {
    return false;
  }
  *out = widened;
  return true;
}

// Copies the selected rows of a flat `column` into `out` through `convert`,
// which returns false for a value it cannot convert. A null row is written as
// zero so nothing downstream reads an uninitialised slot.
template <typename From, typename To, typename Convert>
bool WidenAs(const ColumnView& column,
             uint32_t count,
             Convert convert,
             FlexVector<To>* out,
             BitVector* out_validity) {
  const BitVector* validity = column.validity();
  out_validity->ClearAllBits();
  for (uint32_t row = 0; row < count; ++row) {
    uint32_t index = column.selection().GetIndex(row);
    if (validity && !validity->is_set(index)) {
      (*out)[row] = To{};
      continue;
    }
    if (!convert(column.Value<From>(row), &(*out)[row])) {
      return false;
    }
    out_validity->set(row);
  }
  return true;
}

// The variant counterpart of WidenAs: a row is null when its cell is.
template <typename To, typename Convert>
bool Fill(const ColumnView& column,
          uint32_t count,
          Convert convert,
          FlexVector<To>* out,
          BitVector* out_validity) {
  out_validity->ClearAllBits();
  for (uint32_t row = 0; row < count; ++row) {
    Variant cell = column.Value<Variant>(row);
    if (cell.type == Variant::Type::kNull) {
      (*out)[row] = To{};
      continue;
    }
    if (!convert(cell, &(*out)[row])) {
      return false;
    }
    out_validity->set(row);
  }
  return true;
}

}  // namespace

AssertType::AssertType(uint32_t column, AssertTypeTarget type, std::string name)
    : column_(column),
      target_(type),
      type_(type.Upcast<StorageType>()),
      name_(std::move(name)) {}

AssertType::~AssertType() = default;

AssertType::State::~State() = default;

const void* AssertType::Data(const ColumnChunk& chunk) const {
  switch (target_.index()) {
    case AssertTypeTarget::GetTypeIndex<Int64>():
      return chunk.Values<int64_t>().data();
    case AssertTypeTarget::GetTypeIndex<Double>():
      return chunk.Values<double>().data();
    case AssertTypeTarget::GetTypeIndex<String>():
      return chunk.Values<StringPool::Id>().data();
    default:
      PERFETTO_FATAL("Unreachable");
  }
}

std::unique_ptr<OperatorState> AssertType::MakeState() const {
  auto state = std::make_unique<State>();
  switch (target_.index()) {
    case AssertTypeTarget::GetTypeIndex<Int64>():
      state->chunk.Values<int64_t>();
      break;
    case AssertTypeTarget::GetTypeIndex<Double>():
      state->chunk.Values<double>();
      break;
    case AssertTypeTarget::GetTypeIndex<String>():
      state->chunk.Values<StringPool::Id>();
      break;
    default:
      PERFETTO_FATAL("Unreachable");
  }
  state->chunk.validity = BitVector::CreateWithSize(kMaxBatchRows);
  return state;
}

base::Status AssertType::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

void AssertType::Rewind(OperatorState& state) const {
  state.Cast<State>().status = base::OkStatus();
}

bool AssertType::Widen(const ColumnView& column,
                       uint32_t count,
                       State& state) const {
  ColumnChunk& chunk = state.chunk;
  StorageType from = column.type();
  if (type_.Is<Int64>()) {
    PERFETTO_DCHECK((from.IsAnyOf<base::TypeSet<Id, Uint32, Int32>>()));
    if (from.Is<Int32>()) {
      return WidenAs<int32_t>(column, count, Cast<int32_t, int64_t>,
                              &chunk.Values<int64_t>(), &chunk.validity);
    }
    return WidenAs<uint32_t>(column, count, Cast<uint32_t, int64_t>,
                             &chunk.Values<int64_t>(), &chunk.validity);
  }
  PERFETTO_DCHECK(type_.Is<Double>());
  if (from.Is<Int32>()) {
    return WidenAs<int32_t>(column, count, Cast<int32_t, double>,
                            &chunk.Values<double>(), &chunk.validity);
  }
  if (from.Is<Int64>()) {
    auto exact = [&](int64_t value, double* out) {
      if (ExactDouble(value, out)) {
        return true;
      }
      state.status = NotExact(name_);
      return false;
    };
    return WidenAs<int64_t>(column, count, exact, &chunk.Values<double>(),
                            &chunk.validity);
  }
  PERFETTO_DCHECK(from.Is<Id>() || from.Is<Uint32>());
  return WidenAs<uint32_t>(column, count, Cast<uint32_t, double>,
                           &chunk.Values<double>(), &chunk.validity);
}

OpResult AssertType::Execute(const RowBatch& in,
                             RowBatch& out,
                             OperatorState& state) const {
  State& s = state.Cast<State>();
  out.CopyFrom(in);
  const ColumnView& column = in.column(column_);
  ColumnChunk& chunk = s.chunk;
  if (column.kind() != ColumnView::Kind::kVariant) {
    if (column.type() == type_) {
      return OpResult::kNeedMoreInput;
    }
    bool widen = column.type().IsAnyOf<IntegerType>() &&
                 (type_.Is<Int64>() || type_.Is<Double>());
    if (!widen) {
      s.status = base::ErrStatus("column '%s' is %s, not %s", name_.c_str(),
                                 Name(column.type()), Name(type_));
      return OpResult::kError;
    }
    if (!Widen(column, in.size(), s)) {
      return OpResult::kError;
    }
    // A column without validity has no nulls to remap, so stays non-null.
    const BitVector* validity = column.validity() ? &chunk.validity : nullptr;
    out.SetColumn(column_, ColumnView::Reference(type_, Data(chunk), validity));
    return OpResult::kNeedMoreInput;
  }

  auto mismatch = [&](const Variant& cell) {
    s.status = base::ErrStatus("column '%s' holds %s, not %s", name_.c_str(),
                               Name(cell.type), Name(type_));
    return false;
  };
  uint32_t count = in.size();
  bool ok;
  switch (target_.index()) {
    case AssertTypeTarget::GetTypeIndex<Int64>():
      ok = Fill(
          column, count,
          [&](const Variant& cell, int64_t* out) {
            if (cell.type != Variant::Type::kInt64) {
              return mismatch(cell);
            }
            *out = cell.AsInt64();
            return true;
          },
          &chunk.Values<int64_t>(), &chunk.validity);
      break;
    case AssertTypeTarget::GetTypeIndex<Double>():
      ok = Fill(
          column, count,
          [&](const Variant& cell, double* out) {
            if (cell.type == Variant::Type::kDouble) {
              *out = cell.AsDouble();
              return true;
            }
            if (cell.type != Variant::Type::kInt64) {
              return mismatch(cell);
            }
            if (ExactDouble(cell.AsInt64(), out)) {
              return true;
            }
            s.status = NotExact(name_);
            return false;
          },
          &chunk.Values<double>(), &chunk.validity);
      break;
    case AssertTypeTarget::GetTypeIndex<String>():
      ok = Fill(
          column, count,
          [&](const Variant& cell, StringPool::Id* out) {
            if (cell.type != Variant::Type::kString) {
              return mismatch(cell);
            }
            *out = cell.AsString();
            return true;
          },
          &chunk.Values<StringPool::Id>(), &chunk.validity);
      break;
    default:
      PERFETTO_FATAL("Unreachable");
  }
  if (!ok) {
    return OpResult::kError;
  }
  out.SetColumn(column_,
                ColumnView::Reference(type_, Data(chunk), &chunk.validity));
  return OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
