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

#ifndef SRC_TRACE_PROCESSOR_DB_OVERLAYS_ARRANGEMENT_OVERLAY_H_
#define SRC_TRACE_PROCESSOR_DB_OVERLAYS_ARRANGEMENT_OVERLAY_H_

#include "src/trace_processor/db/overlays/storage_overlay.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {

// Overlay responsible for arranging the elements of Storage. It deals with
// duplicates, permutations and selection. For selection only it's more
// efficient to use `SelectorOverlay`.
class ArrangementOverlay : public StorageOverlay {
 public:
  explicit ArrangementOverlay(const std::vector<uint32_t>* arrangement)
      : arrangement_(std::move(arrangement)) {}

  StorageRange MapToStorageRange(TableRange) const override;

  TableRangeOrBitVector MapToTableRangeOrBitVector(StorageRange,
                                                   OverlayOp) const override;

  TableBitVector MapToTableBitVector(StorageBitVector,
                                     OverlayOp) const override;

  BitVector IsStorageLookupRequired(OverlayOp,
                                    const TableIndexVector&) const override;

  StorageIndexVector MapToStorageIndexVector(TableIndexVector) const override;

  BitVector IndexSearch(OverlayOp, const TableIndexVector&) const override;

  CostEstimatePerRow EstimateCostPerRow(OverlayOp) const override;

 private:
  const std::vector<uint32_t>* arrangement_;
};

}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_OVERLAYS_ARRANGEMENT_OVERLAY_H_
