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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_ARROW_IPC_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_ARROW_IPC_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::core::dataframe {

using ArrowIpcWriteSink = std::function<void(const uint8_t*, size_t)>;

// Reusable Arrow IPC file writer. Keeps scratch buffers across calls to avoid
// repeated allocation when serializing multiple tables.
//
// Usage:
//   ArrowWriter writer;
//   for (auto& table : tables) {
//     size_t size = writer.Prepare(table, pool);
//     // ... write tar header using size ...
//     writer.Write(table, pool, sink);
//   }
class ArrowWriter {
 public:
  struct ColInfo {
    uint32_t idx;
    std::string name;
    bool nullable;
    StorageType storage_type;
  };

  // Prepares serialization metadata for |df|. Returns the exact total byte
  // size of the Arrow IPC file that Write() will produce. This is cheap:
  // it iterates string lengths and builds small flatbuffer metadata, but
  // does not copy any column data.
  size_t Prepare(const Dataframe& df, StringPool* pool);

  // Writes the Arrow IPC file to |sink| using the metadata from the most
  // recent Prepare() call. Must be called on the same df/pool.
  base::Status Write(const Dataframe& df,
                     StringPool* pool,
                     const ArrowIpcWriteSink& sink);

 private:
  // Pre-built header (magic + schema msg + record batch metadata).
  std::vector<uint8_t> header_;
  // Pre-built trailer (footer + footer_size + magic).
  std::vector<uint8_t> trailer_;
  // Column metadata from Prepare().
  std::vector<ColInfo> cols_;
  // Per-column string data length (only meaningful for string columns).
  std::vector<uint32_t> string_data_lens_;
  // Reusable scratch buffer for building bitmaps, string buffers, etc.
  // FlexVector avoids zeroing on resize, unlike std::vector.
  FlexVector<uint8_t> scratch_;

  uint32_t body_size_ = 0;
  uint32_t padded_body_ = 0;
};

// Deserialize an Arrow IPC file into an existing (empty) Dataframe.
// Schema must match exactly (same column names, types, nullability,
// excluding _auto_id).
base::Status DeserializeFromArrowIpc(Dataframe& df,
                                     StringPool* pool,
                                     const util::TraceBlobViewReader& reader);

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_ARROW_IPC_H_
