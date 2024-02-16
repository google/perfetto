/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/db/column/data_layer.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column/arrangement_overlay.h"
#include "src/trace_processor/db/column/dense_null_overlay.h"
#include "src/trace_processor/db/column/dummy_storage.h"
#include "src/trace_processor/db/column/id_storage.h"
#include "src/trace_processor/db/column/null_overlay.h"
#include "src/trace_processor/db/column/numeric_storage.h"
#include "src/trace_processor/db/column/range_overlay.h"
#include "src/trace_processor/db/column/selector_overlay.h"
#include "src/trace_processor/db/column/set_id_storage.h"
#include "src/trace_processor/db/column/string_storage.h"
#include "src/trace_processor/db/column/types.h"

namespace perfetto::trace_processor::column {

DataLayer::~DataLayer() = default;
DataLayerChain::~DataLayerChain() = default;

// All the below code exists as machinery to allow dead-code-elimination
// and linker symbol stripping to work for trace processor built into Chrome. It
// is ugly and hacky but is the only way we could come up with to actually meet
// both performance constraints and saving binary size in Chrome.
//
// TODO(b/325583551): investigate whether we can improve this at some point,
// potentially removing this if Chrome no longer relies on trace processor for
// JSON export.

std::unique_ptr<DataLayerChain> DataLayer::MakeChain() {
  switch (impl_) {
    case Impl::kDummy:
      return static_cast<DummyStorage*>(this)->MakeChain();
    case Impl::kId:
      return static_cast<IdStorage*>(this)->MakeChain();
    case Impl::kNumericDouble:
      return static_cast<NumericStorage<double>*>(this)->MakeChain();
    case Impl::kNumericUint32:
      return static_cast<NumericStorage<uint32_t>*>(this)->MakeChain();
    case Impl::kNumericInt32:
      return static_cast<NumericStorage<int32_t>*>(this)->MakeChain();
    case Impl::kNumericInt64:
      return static_cast<NumericStorage<int64_t>*>(this)->MakeChain();
    case Impl::kSetId:
      return static_cast<SetIdStorage*>(this)->MakeChain();
    case Impl::kString:
      return static_cast<StringStorage*>(this)->MakeChain();
    case Impl::kArrangement:
    case Impl::kDenseNull:
    case Impl::kNull:
    case Impl::kRange:
    case Impl::kSelector:
      PERFETTO_FATAL(
          "Unexpected call to MakeChain(). MakeChain(DataLayerChain) should be "
          "called instead");
  }
  PERFETTO_FATAL("For GCC");
}

std::unique_ptr<DataLayerChain> DataLayer::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs args) {
  switch (impl_) {
    case Impl::kArrangement:
      return static_cast<ArrangementOverlay*>(this)->MakeChain(std::move(inner),
                                                               args);
    case Impl::kDenseNull:
      return static_cast<DenseNullOverlay*>(this)->MakeChain(std::move(inner),
                                                             args);
    case Impl::kNull:
      return static_cast<NullOverlay*>(this)->MakeChain(std::move(inner), args);
    case Impl::kRange:
      return static_cast<RangeOverlay*>(this)->MakeChain(std::move(inner),
                                                         args);
    case Impl::kSelector:
      return static_cast<SelectorOverlay*>(this)->MakeChain(std::move(inner),
                                                            args);
    case Impl::kDummy:
    case Impl::kId:
    case Impl::kNumericDouble:
    case Impl::kNumericUint32:
    case Impl::kNumericInt32:
    case Impl::kNumericInt64:
    case Impl::kSetId:
    case Impl::kString:
      PERFETTO_FATAL(
          "Unexpected call to MakeChain(DataLayerChain). MakeChain() should be "
          "called instead");
  }
  PERFETTO_FATAL("For GCC");
}

ArrangementOverlay::ArrangementOverlay(const std::vector<uint32_t>* arrangement,
                                       Indices::State arrangement_state)
    : DataLayer(Impl::kArrangement),
      arrangement_(arrangement),
      arrangement_state_(arrangement_state) {}
ArrangementOverlay::~ArrangementOverlay() = default;

std::unique_ptr<DataLayerChain> ArrangementOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs args) {
  return std::make_unique<ChainImpl>(std::move(inner), arrangement_,
                                     arrangement_state_,
                                     args.does_layer_order_chain_contents);
}

DenseNullOverlay::DenseNullOverlay(const BitVector* non_null)
    : DataLayer(Impl::kDenseNull), non_null_(non_null) {}
DenseNullOverlay::~DenseNullOverlay() = default;

std::unique_ptr<DataLayerChain> DenseNullOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs) {
  return std::make_unique<ChainImpl>(std::move(inner), non_null_);
}

std::unique_ptr<DataLayerChain> DummyStorage::MakeChain() {
  return std::make_unique<ChainImpl>();
}

IdStorage::IdStorage() : DataLayer(Impl::kId) {}
IdStorage::~IdStorage() = default;

std::unique_ptr<DataLayerChain> IdStorage::MakeChain() {
  return std::make_unique<ChainImpl>();
}

NullOverlay::NullOverlay(const BitVector* non_null)
    : DataLayer(Impl::kNull), non_null_(non_null) {}
NullOverlay::~NullOverlay() = default;

std::unique_ptr<DataLayerChain> NullOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs) {
  return std::make_unique<ChainImpl>(std::move(inner), non_null_);
}

NumericStorageBase::NumericStorageBase(ColumnType type,
                                       bool is_sorted,
                                       Impl impl)
    : DataLayer(impl), storage_type_(type), is_sorted_(is_sorted) {}

NumericStorageBase::~NumericStorageBase() = default;

template <typename T>
std::unique_ptr<DataLayerChain> NumericStorage<T>::MakeChain() {
  return std::make_unique<ChainImpl>(vector_, storage_type_, is_sorted_);
}

template <typename T>
NumericStorage<T>::NumericStorage(const std::vector<T>* vec,
                                  ColumnType type,
                                  bool is_sorted)
    : NumericStorageBase(type, is_sorted, GetImpl()), vector_(vec) {}

// Define explicit instantiation of the necessary templates here to reduce
// binary size bloat.
template class NumericStorage<double>;
template class NumericStorage<uint32_t>;
template class NumericStorage<int32_t>;
template class NumericStorage<int64_t>;

RangeOverlay::RangeOverlay(const Range* range)
    : DataLayer(Impl::kRange), range_(range) {}
RangeOverlay::~RangeOverlay() = default;

std::unique_ptr<DataLayerChain> RangeOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs) {
  return std::make_unique<ChainImpl>(std::move(inner), range_);
}

SelectorOverlay::SelectorOverlay(const BitVector* selector)
    : DataLayer(Impl::kSelector), selector_(selector) {}
SelectorOverlay::~SelectorOverlay() = default;

std::unique_ptr<DataLayerChain> SelectorOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs) {
  return std::make_unique<ChainImpl>(std::move(inner), selector_);
}

SetIdStorage::SetIdStorage(const std::vector<uint32_t>* values)
    : DataLayer(Impl::kSetId), values_(values) {}
SetIdStorage::~SetIdStorage() = default;

std::unique_ptr<DataLayerChain> SetIdStorage::MakeChain() {
  return std::make_unique<ChainImpl>(values_);
}

StringStorage::StringStorage(StringPool* string_pool,
                             const std::vector<StringPool::Id>* data,
                             bool is_sorted)
    : DataLayer(Impl::kString),
      data_(data),
      string_pool_(string_pool),
      is_sorted_(is_sorted) {}
StringStorage::~StringStorage() = default;

std::unique_ptr<DataLayerChain> StringStorage::MakeChain() {
  return std::make_unique<ChainImpl>(string_pool_, data_, is_sorted_);
}

}  // namespace perfetto::trace_processor::column
