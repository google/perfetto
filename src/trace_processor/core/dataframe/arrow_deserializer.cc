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

#include "src/trace_processor/core/dataframe/arrow_deserializer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/arrow_internal.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/util/flatbuffer_reader.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

using util::FlatBufferReader;

using namespace arrow_internal;

bool NeedsPopcount(Nullability type) {
  return type.Is<core::SparseNullWithPopcountAlways>() ||
         type.Is<core::SparseNullWithPopcountUntilFinalization>();
}

class BodyReader {
 public:
  BodyReader(const util::TraceBlobViewReader& data,
             size_t body_offset,
             util::FlatBufferScalarVec<ArrowBuffer> buffers)
      : data_(data), body_offset_(body_offset), buffers_(buffers) {}

  base::StatusOr<TraceBlobView> ReadExact(uint64_t expected_size) {
    return Read(expected_size, true);
  }

  base::Status ReadEmpty() {
    auto buffer = ReadExact(0);
    return buffer.ok() ? base::OkStatus() : buffer.status();
  }

  base::StatusOr<TraceBlobView> ReadVariableSize() { return Read(0, false); }

 private:
  base::StatusOr<TraceBlobView> Read(uint64_t expected_size, bool exact) {
    if (next_buffer_ >= buffers_.size()) {
      return InvalidFile();
    }
    ArrowBuffer buffer = buffers_[next_buffer_++];
    uint64_t size = static_cast<uint64_t>(buffer.length);
    if ((exact && size != expected_size) || (!exact && size < expected_size)) {
      return InvalidFile();
    }
    if (!size) {
      return TraceBlobView{};
    }
    auto blob =
        data_.SliceOff(body_offset_ + static_cast<size_t>(buffer.offset),
                       static_cast<size_t>(buffer.length));
    return blob ? base::StatusOr<TraceBlobView>(std::move(*blob))
                : base::StatusOr<TraceBlobView>(InvalidFile());
  }

  const util::TraceBlobViewReader& data_;
  size_t body_offset_;
  util::FlatBufferScalarVec<ArrowBuffer> buffers_;
  uint32_t next_buffer_ = 0;
};

base::StatusOr<BitVector> ReadValidityBuffer(BodyReader* reader,
                                             uint32_t rows,
                                             int64_t expected_nulls,
                                             bool nullable) {
  if (!nullable) {
    RETURN_IF_ERROR(reader->ReadEmpty());
    return expected_nulls == 0 ? base::StatusOr<BitVector>(BitVector{})
                               : base::StatusOr<BitVector>(InvalidFile());
  }

  ASSIGN_OR_RETURN(TraceBlobView bitmap,
                   reader->ReadExact(ValidityBufferSize(rows)));
  BitVector validity = BitVector::CreateWithSize(rows, false);
  int64_t nulls = rows;
  for (uint32_t row = 0; row < rows; ++row) {
    if (bitmap.data()[BitmapByteIndex(row)] & BitmapMask(row)) {
      validity.set(row);
      --nulls;
    }
  }
  return nulls == expected_nulls
             ? base::StatusOr<BitVector>(std::move(validity))
             : base::StatusOr<BitVector>(InvalidFile());
}

// Reverses WriteUtf8Buffers: validate Arrow's offsets, intern each string, and
// store the resulting StringPool IDs in dense or compact sparse storage.
base::Status ReadUtf8Buffers(BodyReader* reader,
                             uint32_t rows,
                             bool nullable,
                             bool sparse,
                             const BitVector& validity,
                             StringPool* pool,
                             Storage* storage) {
  ASSIGN_OR_RETURN(TraceBlobView offsets,
                   reader->ReadExact(Utf8OffsetBufferSize(rows)));
  ASSIGN_OR_RETURN(TraceBlobView strings, reader->ReadVariableSize());
  if (Load<int32_t>(offsets.data(), 0) != 0) {
    return InvalidFile();
  }
  auto& output = storage->unchecked_get<String>();
  for (uint32_t row = 0; row < rows; ++row) {
    int32_t start = Load<int32_t>(offsets.data(), row);
    int32_t end = Load<int32_t>(offsets.data(), row + 1);
    if (start < 0 || end < start ||
        static_cast<uint64_t>(end) > strings.size()) {
      return InvalidFile();
    }
    if (!nullable || validity.is_set(row)) {
      output.push_back(pool->InternString(base::StringView(
          reinterpret_cast<const char*>(strings.data()) + start,
          static_cast<size_t>(end - start))));
    } else if (!sparse) {
      output.push_back({});
    }
  }
  return Load<int32_t>(offsets.data(), rows) ==
                 static_cast<int64_t>(strings.size())
             ? base::OkStatus()
             : InvalidFile();
}

template <typename T>
void ReadNumericValues(const TraceBlobView& values,
                       uint32_t rows,
                       bool sparse,
                       const BitVector& validity,
                       FlexVector<T>* output) {
  if (!sparse) {
    output->resize(rows);
    if (rows) {
      memcpy(output->data(), values.data(),
             static_cast<size_t>(rows) * sizeof(T));
    }
    return;
  }
  // Arrow has one value slot per logical row. Sparse dataframe storage keeps
  // only valid values, so compact the Arrow buffer using its validity bitmap.
  for (uint32_t row = 0; row < rows; ++row) {
    if (validity.is_set(row)) {
      output->push_back(Load<T>(values.data(), row));
    }
  }
}

base::Status ReadNumericBuffer(BodyReader* reader,
                               uint32_t rows,
                               bool sparse,
                               const BitVector& validity,
                               Storage* storage) {
  ASSIGN_OR_RETURN(TraceBlobView values,
                   reader->ReadExact(static_cast<uint64_t>(rows) *
                                     NumericSize(storage->type())));
  if (storage->type().Is<core::Double>()) {
    ReadNumericValues(values, rows, sparse, validity,
                      &storage->unchecked_get<Double>());
  } else if (storage->type().Is<core::Int64>()) {
    ReadNumericValues(values, rows, sparse, validity,
                      &storage->unchecked_get<Int64>());
  } else if (storage->type().Is<core::Int32>()) {
    ReadNumericValues(values, rows, sparse, validity,
                      &storage->unchecked_get<Int32>());
  } else {
    ReadNumericValues(values, rows, sparse, validity,
                      &storage->unchecked_get<Uint32>());
  }
  return base::OkStatus();
}

void InstallValidity(Nullability nullability,
                     BitVector validity,
                     NullStorage* storage) {
  if (nullability.Is<core::DenseNull>()) {
    storage->unchecked_get<core::DenseNull>().bit_vector = std::move(validity);
  } else if (IsSparse(nullability)) {
    auto& nulls = storage->unchecked_get<core::SparseNull>();
    nulls.bit_vector = std::move(validity);
    if (NeedsPopcount(nullability)) {
      nulls.prefix_popcount_for_cell_get =
          nulls.bit_vector.PrefixPopcountFlexVector();
    }
  }
}

struct LocatedRecordBatch {
  Block block;
  size_t message_offset;
};

// Phase 1: validate the outer file framing and locate the only record batch.
base::StatusOr<LocatedRecordBatch> LocateRecordBatch(
    const util::TraceBlobViewReader& data) {
  size_t base = data.start_offset();
  size_t file_size = data.end_offset() - base;
  if (file_size < kMinimumFileSize) {
    return InvalidFile();
  }

  auto header = data.SliceOff(base, sizeof(kPaddedMagic));
  auto magic = data.SliceOff(base + file_size - sizeof(kMagic), sizeof(kMagic));
  auto footer_size_blob =
      data.SliceOff(base + file_size - kFileTrailerSize, kFooterSizeFieldSize);
  if (!header || !magic || !footer_size_blob ||
      memcmp(header->data(), kPaddedMagic, sizeof(kPaddedMagic)) != 0 ||
      memcmp(magic->data(), kMagic, sizeof(kMagic)) != 0) {
    return InvalidFile();
  }

  uint32_t footer_size;
  memcpy(&footer_size, footer_size_blob->data(), sizeof(footer_size));
  if (static_cast<uint64_t>(footer_size) + kMinimumFileSize > file_size) {
    return InvalidFile();
  }
  size_t footer_offset = base + file_size - kFileTrailerSize - footer_size;
  auto footer_blob = data.SliceOff(footer_offset, footer_size);
  if (!footer_blob) {
    return InvalidFile();
  }

  auto footer = FlatBufferReader::GetRoot(footer_blob->data(), footer_size);
  auto blocks = footer ? footer->VecScalar<Block>(footer_field::kRecordBatches)
                       : util::FlatBufferScalarVec<Block>{};
  if (!footer ||
      footer->Scalar<int16_t>(footer_field::kVersion, kMissingSignedValue) !=
          kMetadataV5 ||
      blocks.size() != kRecordBatchCount) {
    return InvalidFile();
  }

  Block block = blocks[0];
  uint64_t footer_relative = footer_offset - base;
  if (block.offset < 0 || block.metadata_length < 0 ||
      static_cast<uint32_t>(block.metadata_length) < kMessagePrefixSize ||
      block.body_length < 0) {
    return InvalidFile();
  }
  uint64_t block_offset = static_cast<uint64_t>(block.offset);
  uint64_t metadata_length = static_cast<uint32_t>(block.metadata_length);
  uint64_t body_length = static_cast<uint64_t>(block.body_length);
  if (block_offset > footer_relative ||
      metadata_length > footer_relative - block_offset ||
      body_length > footer_relative - block_offset - metadata_length ||
      block_offset + metadata_length + body_length != footer_relative) {
    return InvalidFile();
  }
  return LocatedRecordBatch{block, base + static_cast<size_t>(block_offset)};
}

struct RecordBatchMetadata {
  TraceBlobView blob;
  uint32_t rows;
  size_t body_offset;
  util::FlatBufferScalarVec<FieldNode> nodes;
  util::FlatBufferScalarVec<ArrowBuffer> buffers;
};

// Phase 2: parse and validate the record-batch metadata. Keeping |blob| in the
// result preserves the backing storage referenced by the flatbuffer vectors.
base::StatusOr<RecordBatchMetadata> ReadRecordBatchMetadata(
    const util::TraceBlobViewReader& data,
    const LocatedRecordBatch& located,
    uint32_t expected_nodes,
    uint32_t expected_buffers) {
  uint64_t metadata_length =
      static_cast<uint32_t>(located.block.metadata_length);
  auto prefix = data.SliceOff(located.message_offset, kMessagePrefixSize);
  uint32_t continuation = 0;
  int32_t metadata_size = 0;
  if (prefix) {
    memcpy(&continuation, prefix->data(), sizeof(continuation));
    memcpy(&metadata_size, prefix->data() + sizeof(continuation),
           sizeof(metadata_size));
  }
  if (!prefix || continuation != kContinuation || metadata_size <= 0 ||
      static_cast<uint64_t>(metadata_size) + kMessagePrefixSize !=
          metadata_length) {
    return InvalidFile();
  }

  uint32_t metadata_size_u32 = static_cast<uint32_t>(metadata_size);
  auto metadata = data.SliceOff(located.message_offset + kMessagePrefixSize,
                                metadata_size_u32);
  auto message =
      metadata ? FlatBufferReader::GetRoot(metadata->data(), metadata_size_u32)
               : std::nullopt;
  auto record_batch =
      message ? message->Table(message_field::kHeader) : FlatBufferReader{};
  if (!message ||
      message->Scalar<int16_t>(message_field::kVersion, kMissingSignedValue) !=
          kMetadataV5 ||
      message->Scalar<uint8_t>(message_field::kHeaderType) !=
          kHeaderRecordBatch ||
      !record_batch ||
      message->Scalar<int64_t>(message_field::kBodyLength,
                               kMissingSignedValue) !=
          located.block.body_length) {
    return InvalidFile();
  }

  int64_t row_count = record_batch.Scalar<int64_t>(record_batch_field::kLength,
                                                   kMissingSignedValue);
  auto nodes = record_batch.VecScalar<FieldNode>(record_batch_field::kNodes);
  auto buffers =
      record_batch.VecScalar<ArrowBuffer>(record_batch_field::kBuffers);
  if (row_count < 0 || row_count > std::numeric_limits<uint32_t>::max() ||
      nodes.size() != expected_nodes || buffers.size() != expected_buffers) {
    return InvalidFile();
  }

  uint32_t rows = static_cast<uint32_t>(row_count);
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    FieldNode node = nodes[i];
    if (node.length != rows || node.null_count < 0 || node.null_count > rows) {
      return InvalidFile();
    }
  }
  uint64_t body_length = static_cast<uint64_t>(located.block.body_length);
  for (uint32_t i = 0; i < buffers.size(); ++i) {
    ArrowBuffer buffer = buffers[i];
    if (buffer.offset < 0 || buffer.length < 0 ||
        static_cast<uint64_t>(buffer.offset) > body_length ||
        static_cast<uint64_t>(buffer.length) >
            body_length - static_cast<uint64_t>(buffer.offset)) {
      return InvalidFile();
    }
  }

  size_t body_offset =
      located.message_offset + static_cast<size_t>(metadata_length);
  return RecordBatchMetadata{std::move(*metadata), rows, body_offset, nodes,
                             buffers};
}

}  // namespace

base::StatusOr<Dataframe> DeserializeFromArrow(
    const util::TraceBlobViewReader& data,
    StringPool* pool,
    const DataframeSpec& spec) {
  if (spec.column_names.size() != spec.column_specs.size() ||
      spec.column_names.size() > std::numeric_limits<uint32_t>::max()) {
    return base::ErrStatus("Invalid dataframe spec");
  }
  std::vector<const char*> column_names;
  column_names.reserve(spec.column_names.size());
  for (const std::string& name : spec.column_names) {
    column_names.push_back(name.c_str());
  }
  Dataframe dataframe(pool, static_cast<uint32_t>(column_names.size()),
                      column_names.data(), spec.column_specs.data());

  // The dataframe spec supplies the schema. Derive the exact node and buffer
  // counts for the supported layout before parsing file metadata.
  uint32_t expected_nodes = 0;
  uint32_t expected_buffers = 0;
  for (const auto& column : dataframe.columns_) {
    if (!column->storage.type().Is<core::Id>()) {
      ++expected_nodes;
      expected_buffers += column->storage.type().Is<core::String>()
                              ? kUtf8BufferCount
                              : kFixedWidthBufferCount;
    }
  }

  ASSIGN_OR_RETURN(LocatedRecordBatch located, LocateRecordBatch(data));
  ASSIGN_OR_RETURN(
      RecordBatchMetadata batch,
      ReadRecordBatchMetadata(data, located, expected_nodes, expected_buffers));

  // Phase 3: decode buffers in the same validity-then-values order used by the
  // serializer. Id columns are reconstructed from the record-batch row count
  // because they are implicit and therefore absent from Arrow.
  BodyReader reader(data, batch.body_offset, batch.buffers);
  uint32_t serialized_column = 0;
  for (uint32_t column_index = 0; column_index < dataframe.column_count();
       ++column_index) {
    auto& column = *dataframe.columns_[column_index];
    if (column.storage.type().Is<core::Id>()) {
      column.storage.unchecked_get<Id>().size = batch.rows;
      continue;
    }
    FieldNode node = batch.nodes[serialized_column++];
    Nullability nullability = column.null_storage.nullability();
    bool nullable = IsNullable(nullability);
    bool sparse = IsSparse(nullability);
    ASSIGN_OR_RETURN(
        BitVector validity,
        ReadValidityBuffer(&reader, batch.rows, node.null_count, nullable));
    if (column.storage.type().Is<core::String>()) {
      RETURN_IF_ERROR(ReadUtf8Buffers(&reader, batch.rows, nullable, sparse,
                                      validity, pool, &column.storage));
    } else {
      RETURN_IF_ERROR(ReadNumericBuffer(&reader, batch.rows, sparse, validity,
                                        &column.storage));
    }
    InstallValidity(nullability, std::move(validity), &column.null_storage);
  }
  dataframe.row_count_ = batch.rows;
  ++dataframe.non_column_mutations_;
  dataframe.Finalize();
  return dataframe;
}

}  // namespace perfetto::trace_processor::core::dataframe
