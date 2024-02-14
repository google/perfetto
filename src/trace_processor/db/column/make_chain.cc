/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <memory>
#include <utility>

#include "src/trace_processor/db/column/arrangement_overlay.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/dense_null_overlay.h"
#include "src/trace_processor/db/column/dummy_storage.h"
#include "src/trace_processor/db/column/id_storage.h"
#include "src/trace_processor/db/column/null_overlay.h"
#include "src/trace_processor/db/column/numeric_storage.h"
#include "src/trace_processor/db/column/range_overlay.h"
#include "src/trace_processor/db/column/selector_overlay.h"
#include "src/trace_processor/db/column/set_id_storage.h"
#include "src/trace_processor/db/column/string_storage.h"

// This file contains the implementation of MakeChain for all the
// DataLayer implementations. They are all centralised here because
// there is an alternative set of implementations (see make_chain_minimal.cc)
// the "minimal" target used by export_json in Chrome.

namespace perfetto::trace_processor::column {

std::unique_ptr<DataLayerChain> ArrangementOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs args) {
  return std::make_unique<ChainImpl>(std::move(inner), arrangement_,
                                     arrangement_state_,
                                     args.does_layer_order_chain_contents);
}

std::unique_ptr<DataLayerChain> DenseNullOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs) {
  return std::make_unique<ChainImpl>(std::move(inner), non_null_);
}

std::unique_ptr<DataLayerChain> DummyStorage::MakeChain() {
  return std::make_unique<ChainImpl>();
}

std::unique_ptr<DataLayerChain> IdStorage::MakeChain() {
  return std::make_unique<ChainImpl>();
}

std::unique_ptr<DataLayerChain> NullOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs) {
  return std::make_unique<ChainImpl>(std::move(inner), non_null_);
}

template <typename T>
std::unique_ptr<DataLayerChain> NumericStorage<T>::MakeChain() {
  return std::make_unique<ChainImpl>(vector_, storage_type_, is_sorted_);
}
template std::unique_ptr<DataLayerChain> NumericStorage<double>::MakeChain();
template std::unique_ptr<DataLayerChain> NumericStorage<uint32_t>::MakeChain();
template std::unique_ptr<DataLayerChain> NumericStorage<int32_t>::MakeChain();
template std::unique_ptr<DataLayerChain> NumericStorage<int64_t>::MakeChain();

std::unique_ptr<DataLayerChain> RangeOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs) {
  return std::make_unique<ChainImpl>(std::move(inner), range_);
}

std::unique_ptr<DataLayerChain> SelectorOverlay::MakeChain(
    std::unique_ptr<DataLayerChain> inner,
    ChainCreationArgs) {
  return std::make_unique<ChainImpl>(std::move(inner), selector_);
}

std::unique_ptr<DataLayerChain> SetIdStorage::MakeChain() {
  return std::make_unique<ChainImpl>(values_);
}

std::unique_ptr<DataLayerChain> StringStorage::MakeChain() {
  return std::make_unique<ChainImpl>(string_pool_, data_, is_sorted_);
}

}  // namespace perfetto::trace_processor::column
