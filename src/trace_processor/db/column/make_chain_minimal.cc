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
// DataLayer implementations the "minimal" target used by export_json in Chrome.
// See make_chain.cc for the real implementations for these functions.

namespace perfetto::trace_processor::column {

std::unique_ptr<DataLayerChain> ArrangementOverlay::MakeChain(
    std::unique_ptr<DataLayerChain>,
    ChainCreationArgs) {
  return std::make_unique<DummyStorage::ChainImpl>();
}

std::unique_ptr<DataLayerChain> DenseNullOverlay::MakeChain(
    std::unique_ptr<DataLayerChain>,
    ChainCreationArgs) {
  return std::make_unique<DummyStorage::ChainImpl>();
}

std::unique_ptr<DataLayerChain> DummyStorage::MakeChain() {
  return std::make_unique<DummyStorage::ChainImpl>();
}

std::unique_ptr<DataLayerChain> IdStorage::MakeChain() {
  return std::make_unique<DummyStorage::ChainImpl>();
}

std::unique_ptr<DataLayerChain> NullOverlay::MakeChain(
    std::unique_ptr<DataLayerChain>,
    ChainCreationArgs) {
  return std::make_unique<DummyStorage::ChainImpl>();
}

template <typename T>
std::unique_ptr<DataLayerChain> NumericStorage<T>::MakeChain() {
  return std::make_unique<DummyStorage::ChainImpl>();
}
template std::unique_ptr<DataLayerChain> NumericStorage<double>::MakeChain();
template std::unique_ptr<DataLayerChain> NumericStorage<uint32_t>::MakeChain();
template std::unique_ptr<DataLayerChain> NumericStorage<int32_t>::MakeChain();
template std::unique_ptr<DataLayerChain> NumericStorage<int64_t>::MakeChain();

std::unique_ptr<DataLayerChain> RangeOverlay::MakeChain(
    std::unique_ptr<DataLayerChain>,
    ChainCreationArgs) {
  return std::make_unique<DummyStorage::ChainImpl>();
}

std::unique_ptr<DataLayerChain> SelectorOverlay::MakeChain(
    std::unique_ptr<DataLayerChain>,
    ChainCreationArgs) {
  return std::make_unique<DummyStorage::ChainImpl>();
}

std::unique_ptr<DataLayerChain> SetIdStorage::MakeChain() {
  return std::make_unique<DummyStorage::ChainImpl>();
}

std::unique_ptr<DataLayerChain> StringStorage::MakeChain() {
  return std::make_unique<DummyStorage::ChainImpl>();
}

}  // namespace perfetto::trace_processor::column
