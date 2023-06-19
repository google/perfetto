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

#ifndef SRC_TRACE_PROCESSOR_DB_OVERLAYS_NULL_OVERLAY_H_
#define SRC_TRACE_PROCESSOR_DB_OVERLAYS_NULL_OVERLAY_H_

#include "src/trace_processor/db/overlays/storage_overlay.h"
#include "src/trace_processor/db/overlays/types.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {

// Introduces the layer of nullability - spreads out the storage with nulls
// using BitVector.
class NullOverlay : public StorageOverlay {
 public:
  explicit NullOverlay(const BitVector* null) : non_null_(std::move(null)) {}

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
  // Non null data in the overlay.
  const BitVector* non_null_;
};

}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_OVERLAYS_NULL_OVERLAY_H_
