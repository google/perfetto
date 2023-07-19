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

#ifndef SRC_TRACE_PROCESSOR_DB_OVERLAYS_STORAGE_OVERLAY_H_
#define SRC_TRACE_PROCESSOR_DB_OVERLAYS_STORAGE_OVERLAY_H_

#include "src/trace_processor/db/overlays/types.h"

namespace perfetto {
namespace trace_processor {
namespace overlays {

// Abstract class which is layered on top of Storage transforming how the
// storage should be interpreted. The main purpose of this class is to be
// responsible for for mapping between table indices and storage indices (i.e.
// in both directions).
//
// Overlays are designed to be "layered" on top of each other (i.e. the mapping
// algorithms compose). To make it easier to reason about this class, we
// ignore any other overlays and assume we are mapping directly between table
// indices and storage indices. i.e. even if "table indices" we are working with
// come from another overlay, we still consider them as having come from the
// table and vice versa for "storage indices".
//
// The core functions in this class work with input and output arguments which
// use the same data structure but have different semantics (i.e. input might
// be in terms of storage indices and output might be in terms of table
// indices).
//
// For this reason, we use the defined wrapper structs which "tag" the data
// structure with the semantics.
class StorageOverlay {
 public:
  virtual ~StorageOverlay();

  // Maps a range of indices in table space to an equivalent range of
  // indices in the storage space.
  virtual StorageRange MapToStorageRange(TableRange) const = 0;

  // Returns the smallest Range or BitVector containing all of the elements
  // matching the OverlayOp.
  virtual TableRangeOrBitVector MapToTableRangeOrBitVector(StorageRange,
                                                           OverlayOp) const = 0;

  // Maps a BitVector of indices in storage space to an equivalent range of
  // indices in the table space.
  virtual TableBitVector MapToTableBitVector(StorageBitVector,
                                             OverlayOp) const = 0;

  // Returns a BitVector where each boolean indicates if the corresponding index
  // in |indices| needs to be mapped and searched in the storage or if the
  // overlay can provide the answer without storage lookup.
  virtual BitVector IsStorageLookupRequired(OverlayOp,
                                            const TableIndexVector&) const = 0;

  // Maps a vector of indices in the table space with an equivalent range
  // of indices in the storage space.
  //
  // Note: callers must call |IsStorageSearchRequired| first and only call
  // this method with indices where |IsStorageSearchRequired| returned true.
  // Passing indices here which are not mappable is undefined behaviour.
  virtual StorageIndexVector MapToStorageIndexVector(
      TableIndexVector) const = 0;

  // Given a vector of indices given in table space, returns whether the index
  // matches the operation given by |op|.
  //
  // Note: callers must call |IsStorageSearchRequired| first and only call
  // this method with indices where |IsStorageSearchRequired| returned false.
  // Passing indices here which are not searchable is undefined behaviour.
  virtual BitVector IndexSearch(OverlayOp, const TableIndexVector&) const = 0;

  // Estimates the per-row costs of the methods of this class. Allows for
  // deciding which algorithm to use to search/sort the storage.
  virtual CostEstimatePerRow EstimateCostPerRow(OverlayOp) const = 0;
};

}  // namespace overlays
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_OVERLAYS_STORAGE_OVERLAY_H_
