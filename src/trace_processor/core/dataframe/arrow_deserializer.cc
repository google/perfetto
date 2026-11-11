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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
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
#include "src/trace_processor/util/decompressor.h"
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
  // |decompressor| is null for uncompressed batches.
  BodyReader(const util::TraceBlobViewReader& data,
             size_t body_offset,
             util::FlatBufferScalarVec<ArrowBuffer> buffers,
             util::Decompressor* decompressor)
      : data_(data),
        body_offset_(body_offset),
        buffers_(buffers),
        decompressor_(decompressor) {}

  base::StatusOr<TraceBlobView> ReadExact(uint64_t expected_size) {
    return Read(expected_size, true);
  }

  base::Status ReadEmpty() {
    auto buffer = ReadExact(0);
    return buffer.ok() ? base::OkStatus() : buffer.status();
  }

  base::StatusOr<TraceBlobView> ReadVariableSize() { return Read(0, false); }

  // Returns the buffer as the pieces it arrived in. Callers which only copy the
  // bytes out can consume those directly, where ReadExact would first have to
  // join them into one allocation.
  base::StatusOr<std::vector<TraceBlobView>> ReadExactPieces(
      uint64_t expected_size) {
    PERFETTO_DCHECK(!decompressor_);
    ASSIGN_OR_RETURN(ArrowBuffer buffer, Next(expected_size, true));
    auto size = static_cast<size_t>(buffer.length);
    std::vector<TraceBlobView> pieces = data_.MultiSliceOff(
        body_offset_ + static_cast<size_t>(buffer.offset), size);
    size_t total = 0;
    for (const TraceBlobView& piece : pieces) {
      total += piece.size();
    }
    return total == size
               ? base::StatusOr<std::vector<TraceBlobView>>(std::move(pieces))
               : base::StatusOr<std::vector<TraceBlobView>>(InvalidFile());
  }

  base::Status Skip() { return Next(0, false).status(); }

  // Reads a buffer of exactly |size| bytes, copying its first |kept| bytes to
  // |out|.
  base::Status ReadInto(uint8_t* out, uint64_t size, size_t kept) {
    if (!decompressor_) {
      ASSIGN_OR_RETURN(std::vector<TraceBlobView> pieces,
                       ReadExactPieces(size));
      CopyPrefix(pieces, out, kept);
      return base::OkStatus();
    }
    ASSIGN_OR_RETURN(ArrowBuffer buffer, Next(size, true));
    if (!buffer.length) {
      return size == 0 ? base::OkStatus() : InvalidFile();
    }
    size_t start = body_offset_ + static_cast<size_t>(buffer.offset);
    auto compressed = static_cast<size_t>(buffer.length);
    ASSIGN_OR_RETURN(int64_t length, ReadLength(&start, &compressed));
    if (length == kUncompressedBuffer) {
      if (compressed != size) {
        return InvalidFile();
      }
      CopyPrefix(data_.MultiSliceOff(start, compressed), out, kept);
      return base::OkStatus();
    }
    if (static_cast<uint64_t>(length) != size) {
      return InvalidFile();
    }
    return DecompressFrame(start, compressed, out, static_cast<size_t>(size),
                           kept);
  }

 private:
  base::StatusOr<ArrowBuffer> Next(uint64_t expected_size, bool exact) {
    if (next_buffer_ >= buffers_.size()) {
      return InvalidFile();
    }
    ArrowBuffer buffer = buffers_[next_buffer_++];
    // Compressed buffers are checked once decompressed.
    if (!decompressor_ && !SizeMatches(static_cast<uint64_t>(buffer.length),
                                       expected_size, exact)) {
      return InvalidFile();
    }
    return buffer;
  }

  base::StatusOr<TraceBlobView> Read(uint64_t expected_size, bool exact) {
    ASSIGN_OR_RETURN(ArrowBuffer buffer, Next(expected_size, exact));
    if (!buffer.length) {
      return SizeMatches(0, expected_size, exact)
                 ? base::StatusOr<TraceBlobView>(TraceBlobView{})
                 : base::StatusOr<TraceBlobView>(InvalidFile());
    }
    size_t start = body_offset_ + static_cast<size_t>(buffer.offset);
    auto size = static_cast<size_t>(buffer.length);
    int64_t length = kUncompressedBuffer;
    if (decompressor_) {
      ASSIGN_OR_RETURN(length, ReadLength(&start, &size));
    }
    if (length == kUncompressedBuffer) {
      auto blob = data_.SliceOff(start, size);
      if (!blob || !SizeMatches(blob->size(), expected_size, exact)) {
        return InvalidFile();
      }
      return std::move(*blob);
    }
    auto total = static_cast<size_t>(length);
    if (!SizeMatches(total, expected_size, exact)) {
      return InvalidFile();
    }
    TraceBlob blob = TraceBlob::Allocate(total);
    RETURN_IF_ERROR(
        DecompressFrame(start, size, blob.mutable_data(), total, total));
    return TraceBlobView(std::move(blob));
  }

  // A compressed buffer starts with its uncompressed length.
  base::StatusOr<int64_t> ReadLength(size_t* start, size_t* size) {
    int64_t length;
    auto prefix = data_.SliceOff(*start, sizeof(length));
    if (*size < sizeof(length) || !prefix) {
      return InvalidFile();
    }
    memcpy(&length, prefix->data(), sizeof(length));
    if (length < 0 && length != kUncompressedBuffer) {
      return InvalidFile();
    }
    *start += sizeof(length);
    *size -= sizeof(length);
    return length;
  }

  // Decompresses the zstd frame at |start|, feeding the decompressor one input
  // slice at a time. The first |kept| of its |total| bytes go to |out|.
  base::Status DecompressFrame(size_t start,
                               size_t size,
                               uint8_t* out,
                               size_t total,
                               size_t kept) {
    using ResultCode = util::Decompressor::ResultCode;
    uint8_t discard[64 * 1024];
    decompressor_->Reset();
    size_t written = 0;
    ResultCode ret = ResultCode::kNeedsMoreInput;
    std::vector<TraceBlobView> pieces = data_.MultiSliceOff(start, size);
    size_t i = 0;
    for (; i < pieces.size() && ret != ResultCode::kEof; ++i) {
      decompressor_->Feed(pieces[i].data(), pieces[i].size());
      do {
        bool keep = written < kept;
        uint8_t* dest = keep ? out + written : discard;
        size_t capacity =
            keep ? kept - written : std::min(sizeof(discard), total - written);
        auto result = decompressor_->ExtractOutput(dest, capacity);
        if (result.ret == ResultCode::kError) {
          return InvalidFile();
        }
        written += result.bytes_written;
        ret = result.ret;
      } while (ret == ResultCode::kOk);
    }
    if (ret != ResultCode::kEof) {
      return InvalidFile();
    }
    // A padded buffer's padding can be a frame of its own, left undecoded.
    if (written == kept && kept < total) {
      return base::OkStatus();
    }
    return written == total && !decompressor_->AvailIn() && i == pieces.size()
               ? base::OkStatus()
               : InvalidFile();
  }

  static void CopyPrefix(const std::vector<TraceBlobView>& pieces,
                         uint8_t* out,
                         size_t kept) {
    for (const TraceBlobView& piece : pieces) {
      size_t copied = std::min(kept, piece.size());
      memcpy(out, piece.data(), copied);
      out += copied;
      kept -= copied;
    }
  }

  static bool SizeMatches(uint64_t size, uint64_t expected_size, bool exact) {
    return exact ? size == expected_size : size >= expected_size;
  }

  const util::TraceBlobViewReader& data_;
  size_t body_offset_;
  util::FlatBufferScalarVec<ArrowBuffer> buffers_;
  util::Decompressor* decompressor_;
  uint32_t next_buffer_ = 0;
};

// Reads a bitmap of |rows| bits straight into a BitVector's words.
base::StatusOr<BitVector> ReadBitmap(BodyReader* reader, uint32_t rows) {
  auto words = FlexVector<uint64_t>::CreateWithSize(
      (static_cast<uint64_t>(rows) + 63) / 64);
  if (words.size()) {
    words.back() = 0;
  }
  auto bytes = static_cast<size_t>(ValidityBufferSize(rows));
  RETURN_IF_ERROR(
      reader->ReadInto(reinterpret_cast<uint8_t*>(words.data()), bytes, bytes));
  return BitVector::CreateFromWords(std::move(words), rows);
}

base::StatusOr<BitVector> ReadValidityBuffer(BodyReader* reader,
                                             uint32_t rows,
                                             int64_t expected_nulls,
                                             bool nullable) {
  if (!nullable) {
    RETURN_IF_ERROR(reader->ReadEmpty());
    return expected_nulls == 0 ? base::StatusOr<BitVector>(BitVector{})
                               : base::StatusOr<BitVector>(InvalidFile());
  }

  ASSIGN_OR_RETURN(BitVector validity, ReadBitmap(reader, rows));
  int64_t nulls = static_cast<int64_t>(rows) -
                  static_cast<int64_t>(validity.CountSetBits());
  return nulls == expected_nulls
             ? base::StatusOr<BitVector>(std::move(validity))
             : base::StatusOr<BitVector>(InvalidFile());
}

using Dictionary = FlexVector<StringPool::Id>;

// Reverses ArrowSerializer::WriteDictionary.
base::StatusOr<Dictionary> ReadDictionaryValues(BodyReader* reader,
                                                uint32_t count,
                                                StringPool* pool) {
  RETURN_IF_ERROR(reader->ReadEmpty());
  ASSIGN_OR_RETURN(TraceBlobView offsets,
                   reader->ReadExact(Utf8OffsetBufferSize(count)));
  ASSIGN_OR_RETURN(TraceBlobView strings, reader->ReadVariableSize());
  if (Load<int32_t>(offsets.data(), 0) != 0) {
    return InvalidFile();
  }
  const char* bytes =
      strings.size() ? reinterpret_cast<const char*>(strings.data()) : "";
  Dictionary values = Dictionary::CreateWithCapacity(count);
  for (uint32_t i = 0; i < count; ++i) {
    int32_t start = Load<int32_t>(offsets.data(), i);
    int32_t end = Load<int32_t>(offsets.data(), i + 1);
    if (start < 0 || end < start ||
        static_cast<uint64_t>(end) > strings.size()) {
      return InvalidFile();
    }
    values.push_back(pool->InternString(
        base::StringView(bytes + start, static_cast<size_t>(end - start))));
  }
  if (static_cast<uint64_t>(Load<int32_t>(offsets.data(), count)) !=
      strings.size()) {
    return InvalidFile();
  }
  return std::move(values);
}

// Reverses ArrowSerializer::WriteDictionaryIndices.
base::Status ReadDictionaryIndices(BodyReader* reader,
                                   uint32_t rows,
                                   uint32_t stored_rows,
                                   bool nullable,
                                   bool sparse,
                                   const BitVector& validity,
                                   const Dictionary& dictionary,
                                   Storage* storage) {
  ASSIGN_OR_RETURN(
      TraceBlobView indices,
      reader->ReadExact(static_cast<uint64_t>(rows) * sizeof(DictionaryIndex)));
  auto& output = storage->unchecked_get<String>();
  output.reserve(stored_rows);
  for (uint32_t row = 0; row < rows; ++row) {
    if (nullable && !validity.is_set(row)) {
      if (!sparse) {
        output.push_back({});
      }
      continue;
    }
    DictionaryIndex index = Load<DictionaryIndex>(indices.data(), row);
    if (index < 0 || static_cast<uint64_t>(index) >= dictionary.size()) {
      return InvalidFile();
    }
    output.push_back(dictionary[static_cast<uint32_t>(index)]);
  }
  return base::OkStatus();
}

// Reads the indices of a split string column, the first |valid| of |rows|,
// straight into its storage and maps them to string ids in place.
base::Status ReadPackedDictionaryIndices(BodyReader* reader,
                                         uint32_t rows,
                                         uint32_t valid,
                                         const Dictionary& dictionary,
                                         Storage* storage) {
  static_assert(sizeof(StringPool::Id) == sizeof(DictionaryIndex));
  auto& output = storage->unchecked_get<String>();
  output.resize(valid);
  auto* bytes = reinterpret_cast<uint8_t*>(output.data());
  RETURN_IF_ERROR(reader->ReadInto(
      bytes, static_cast<uint64_t>(rows) * sizeof(DictionaryIndex),
      static_cast<size_t>(valid) * sizeof(DictionaryIndex)));
  for (uint32_t i = 0; i < valid; ++i) {
    auto index = Load<DictionaryIndex>(bytes, i);
    if (index < 0 || static_cast<uint64_t>(index) >= dictionary.size()) {
      return InvalidFile();
    }
    output.data()[i] = dictionary[static_cast<uint32_t>(index)];
  }
  return base::OkStatus();
}

template <typename Fn>
void VisitNumericStorage(Storage* storage, Fn fn) {
  if (storage->type().Is<core::Double>()) {
    fn(&storage->unchecked_get<Double>());
  } else if (storage->type().Is<core::Int64>()) {
    fn(&storage->unchecked_get<Int64>());
  } else if (storage->type().Is<core::Int32>()) {
    fn(&storage->unchecked_get<Int32>());
  } else {
    fn(&storage->unchecked_get<Uint32>());
  }
}

// Arrow has one value slot per logical row. Sparse dataframe storage keeps
// only valid values, so compact the Arrow buffer using its validity bitmap.
template <typename T>
void CompactSparseValues(const TraceBlobView& values,
                         uint32_t rows,
                         uint32_t stored_rows,
                         const BitVector& validity,
                         FlexVector<T>* output) {
  output->reserve(stored_rows);
  for (uint32_t row = 0; row < rows; ++row) {
    if (validity.is_set(row)) {
      output->push_back(Load<T>(values.data(), row));
    }
  }
}

base::Status ReadNumericBuffer(BodyReader* reader,
                               uint32_t rows,
                               uint32_t stored_rows,
                               bool sparse,
                               const BitVector& validity,
                               Storage* storage) {
  uint64_t size = static_cast<uint64_t>(rows) * NumericSize(storage->type());
  if (!sparse) {
    base::Status status;
    VisitNumericStorage(storage, [&](auto* output) {
      output->resize(rows);
      status = reader->ReadInto(reinterpret_cast<uint8_t*>(output->data()),
                                size, static_cast<size_t>(size));
    });
    return status;
  }
  ASSIGN_OR_RETURN(TraceBlobView values, reader->ReadExact(size));
  VisitNumericStorage(storage, [&](auto* output) {
    CompactSparseValues(values, rows, stored_rows, validity, output);
  });
  return base::OkStatus();
}

template <typename T>
void FillDefault(uint32_t rows, FlexVector<T>* output) {
  output->resize(rows);
  std::fill_n(output->data(), rows, T{});
}

// Sparse storage keeps no values for null rows; dense needs one per row.
void InstallAllNullStorage(uint32_t rows, bool sparse, Storage* storage) {
  if (sparse) {
    return;
  }
  if (storage->type().Is<core::String>()) {
    FillDefault(rows, &storage->unchecked_get<String>());
    return;
  }
  VisitNumericStorage(storage,
                      [&](auto* output) { FillDefault(rows, output); });
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

struct LocatedBatches {
  std::vector<Block> dictionaries;
  Block block;
  size_t message_offset;
  std::vector<uint8_t> field_types;
};

// Phase 1: validate the outer file framing and locate the dictionary batches
// and the only record batch.
base::StatusOr<LocatedBatches> LocateBatches(
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
  auto dictionary_blocks =
      footer ? footer->VecScalar<Block>(footer_field::kDictionaries)
             : util::FlatBufferScalarVec<Block>{};
  if (!footer ||
      footer->Scalar<int16_t>(footer_field::kVersion, kMissingSignedValue) !=
          kMetadataV5 ||
      blocks.size() != kRecordBatchCount) {
    return InvalidFile();
  }
  auto schema = footer->Table(footer_field::kSchema);
  if (!schema) {
    return InvalidFile();
  }
  auto fields = schema.VecTable(schema_field::kFields);
  std::vector<uint8_t> field_types;
  field_types.reserve(fields.size());
  for (uint32_t i = 0; i < fields.size(); ++i) {
    field_types.push_back(fields[i].Scalar<uint8_t>(field_field::kTypeType));
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
      block_offset + metadata_length + body_length + kEndOfStreamSize !=
          footer_relative) {
    return InvalidFile();
  }

  std::vector<Block> dictionaries;
  dictionaries.reserve(dictionary_blocks.size());
  for (uint32_t i = 0; i < dictionary_blocks.size(); ++i) {
    Block dictionary = dictionary_blocks[i];
    if (dictionary.offset < 0 || dictionary.metadata_length < 0 ||
        static_cast<uint32_t>(dictionary.metadata_length) <
            kMessagePrefixSize ||
        dictionary.body_length < 0) {
      return InvalidFile();
    }
    uint64_t offset = static_cast<uint64_t>(dictionary.offset);
    uint64_t metadata = static_cast<uint32_t>(dictionary.metadata_length);
    uint64_t body = static_cast<uint64_t>(dictionary.body_length);
    if (offset > block_offset || metadata > block_offset - offset ||
        body > block_offset - offset - metadata) {
      return InvalidFile();
    }
    dictionaries.push_back(dictionary);
  }
  auto marker = data.SliceOff(
      base + static_cast<size_t>(footer_relative - kEndOfStreamSize),
      kEndOfStreamSize);
  MessagePrefix end_of_stream{};
  if (!marker) {
    return InvalidFile();
  }
  memcpy(&end_of_stream, marker->data(), sizeof(end_of_stream));
  if (end_of_stream.continuation != kContinuation ||
      end_of_stream.metadata_size != 0) {
    return InvalidFile();
  }
  return LocatedBatches{std::move(dictionaries), block,
                        base + static_cast<size_t>(block_offset),
                        std::move(field_types)};
}

struct RecordBatchMetadata {
  TraceBlobView blob;
  uint32_t rows;
  size_t body_offset;
  util::FlatBufferScalarVec<FieldNode> nodes;
  util::FlatBufferScalarVec<ArrowBuffer> buffers;
  bool compressed;
};

struct Message {
  TraceBlobView blob;
  FlatBufferReader message;
  size_t body_offset;
};

base::StatusOr<Message> ReadMessage(const util::TraceBlobViewReader& data,
                                    const Block& block,
                                    size_t message_offset,
                                    uint8_t expected_header) {
  uint64_t metadata_length = static_cast<uint32_t>(block.metadata_length);
  auto prefix = data.SliceOff(message_offset, kMessagePrefixSize);
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
  auto metadata =
      data.SliceOff(message_offset + kMessagePrefixSize, metadata_size_u32);
  auto message =
      metadata ? FlatBufferReader::GetRoot(metadata->data(), metadata_size_u32)
               : std::nullopt;
  if (!message ||
      message->Scalar<int16_t>(message_field::kVersion, kMissingSignedValue) !=
          kMetadataV5 ||
      message->Scalar<uint8_t>(message_field::kHeaderType) != expected_header ||
      message->Scalar<int64_t>(message_field::kBodyLength,
                               kMissingSignedValue) != block.body_length) {
    return InvalidFile();
  }
  return Message{std::move(*metadata), *message,
                 message_offset + static_cast<size_t>(metadata_length)};
}

// Phase 2: validate the shape of a record batch against the layout the caller
// derived from the dataframe spec.
base::StatusOr<RecordBatchMetadata> ReadRecordBatch(
    Message message,
    const FlatBufferReader& record_batch,
    int64_t body_length,
    std::optional<uint32_t> expected_rows,
    uint32_t expected_nodes,
    uint32_t expected_buffers) {
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
  if (expected_rows && rows != *expected_rows) {
    return InvalidFile();
  }
  for (uint32_t i = 0; i < nodes.size(); ++i) {
    FieldNode node = nodes[i];
    if (node.length != rows || node.null_count < 0 || node.null_count > rows) {
      return InvalidFile();
    }
  }
  for (uint32_t i = 0; i < buffers.size(); ++i) {
    ArrowBuffer buffer = buffers[i];
    uint64_t body = static_cast<uint64_t>(body_length);
    if (buffer.offset < 0 || buffer.length < 0 ||
        static_cast<uint64_t>(buffer.offset) > body ||
        static_cast<uint64_t>(buffer.length) >
            body - static_cast<uint64_t>(buffer.offset)) {
      return InvalidFile();
    }
  }
  auto compression = record_batch.Table(record_batch_field::kCompression);
  if (compression) {
    if (compression.Scalar<uint8_t>(body_compression_field::kCodec) !=
            kCompressionZstd ||
        compression.Scalar<uint8_t>(body_compression_field::kMethod) !=
            kCompressionMethodBuffer) {
      return InvalidFile();
    }
    if (!util::IsZstdSupported()) {
      return base::ErrStatus(
          "Arrow file is zstd compressed but this build of trace processor "
          "does not support zstd");
    }
  }
  return RecordBatchMetadata{
      std::move(message.blob),       rows, message.body_offset, nodes, buffers,
      static_cast<bool>(compression)};
}

// Returns null for uncompressed batches, creating the decompressor otherwise.
util::Decompressor* DecompressorFor(bool compressed,
                                    std::unique_ptr<util::Decompressor>* zstd) {
  if (!compressed) {
    return nullptr;
  }
  if (!*zstd) {
    *zstd = util::CreateDecompressor(util::CompressionType::kZstd);
  }
  return zstd->get();
}

base::StatusOr<std::vector<Dictionary>> ReadDictionaries(
    const util::TraceBlobViewReader& data,
    const LocatedBatches& located,
    StringPool* pool,
    std::unique_ptr<util::Decompressor>* zstd) {
  size_t base = data.start_offset();
  std::vector<Dictionary> dictionaries;
  dictionaries.reserve(located.dictionaries.size());
  for (uint32_t i = 0; i < located.dictionaries.size(); ++i) {
    const Block& block = located.dictionaries[i];
    ASSIGN_OR_RETURN(
        Message message,
        ReadMessage(data, block, base + static_cast<size_t>(block.offset),
                    kHeaderDictionaryBatch));
    auto batch = message.message.Table(message_field::kHeader);
    if (!batch ||
        batch.Scalar<int64_t>(dictionary_batch_field::kId,
                              kMissingSignedValue) != i ||
        batch.Scalar<bool>(dictionary_batch_field::kIsDelta)) {
      return InvalidFile();
    }
    auto record_batch = batch.Table(dictionary_batch_field::kData);
    if (!record_batch) {
      return InvalidFile();
    }
    ASSIGN_OR_RETURN(
        RecordBatchMetadata values,
        ReadRecordBatch(std::move(message), record_batch, block.body_length,
                        std::nullopt, 1, kUtf8BufferCount));
    BodyReader reader(data, values.body_offset, values.buffers,
                      DecompressorFor(values.compressed, zstd));
    ASSIGN_OR_RETURN(Dictionary dictionary,
                     ReadDictionaryValues(&reader, values.rows, pool));
    dictionaries.push_back(std::move(dictionary));
  }
  return std::move(dictionaries);
}

}  // namespace

base::StatusOr<Dataframe> DeserializeFromArrow(
    const util::TraceBlobViewReader& data,
    StringPool* pool,
    const DataframeSpec& spec,
    ArrowSparseColumns sparse_columns) {
#if PERFETTO_IS_LITTLE_ENDIAN()
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

  ASSIGN_OR_RETURN(LocatedBatches located, LocateBatches(data));
  std::unique_ptr<util::Decompressor> zstd;

  // The spec supplies the schema; the footer says which columns are all-null.
  bool split = sparse_columns == ArrowSparseColumns::kSplit;
  uint32_t expected_nodes = 0;
  uint32_t expected_buffers = 0;
  uint32_t expected_dictionaries = 0;
  for (const auto& column : dataframe.columns_) {
    if (column->storage.type().Is<core::Id>()) {
      continue;
    }
    if (expected_nodes >= located.field_types.size()) {
      return InvalidFile();
    }
    uint8_t field_type = located.field_types[expected_nodes++];
    Nullability nullability = column->null_storage.nullability();
    if (field_type == kTypeNull) {
      if (!IsNullable(nullability)) {
        return InvalidFile();
      }
      continue;
    }
    if (split && IsSparse(nullability)) {
      if (field_type != kTypeBool ||
          expected_nodes >= located.field_types.size() ||
          located.field_types[expected_nodes++] == kTypeNull) {
        return InvalidFile();
      }
      expected_buffers += kFixedWidthBufferCount;
    }
    expected_buffers += kFixedWidthBufferCount;
    expected_dictionaries += column->storage.type().Is<core::String>();
  }
  if (expected_nodes != located.field_types.size() ||
      located.dictionaries.size() != expected_dictionaries) {
    return InvalidFile();
  }
  ASSIGN_OR_RETURN(std::vector<Dictionary> dictionaries,
                   ReadDictionaries(data, located, pool, &zstd));
  ASSIGN_OR_RETURN(Message message,
                   ReadMessage(data, located.block, located.message_offset,
                               kHeaderRecordBatch));
  auto record_batch = message.message.Table(message_field::kHeader);
  if (!record_batch) {
    return InvalidFile();
  }
  ASSIGN_OR_RETURN(RecordBatchMetadata batch,
                   ReadRecordBatch(std::move(message), record_batch,
                                   located.block.body_length, std::nullopt,
                                   expected_nodes, expected_buffers));

  // Phase 3: decode buffers in the same validity-then-values order used by the
  // serializer. Id columns are reconstructed from the record-batch row count
  // because they are implicit and therefore absent from Arrow.
  BodyReader reader(data, batch.body_offset, batch.buffers,
                    DecompressorFor(batch.compressed, &zstd));
  uint32_t serialized_column = 0;
  uint32_t dictionary_index = 0;
  for (uint32_t column_index = 0; column_index < dataframe.column_count();
       ++column_index) {
    auto& column = *dataframe.columns_[column_index];
    if (column.storage.type().Is<core::Id>()) {
      column.storage.unchecked_get<Id>().size = batch.rows;
      continue;
    }
    bool all_null = located.field_types[serialized_column] == kTypeNull;
    FieldNode node = batch.nodes[serialized_column++];
    Nullability nullability = column.null_storage.nullability();
    bool nullable = IsNullable(nullability);
    bool sparse = IsSparse(nullability);
    if (all_null) {
      if (node.null_count != batch.rows) {
        return InvalidFile();
      }
      InstallAllNullStorage(batch.rows, sparse, &column.storage);
      InstallValidity(nullability, BitVector::CreateWithSize(batch.rows),
                      &column.null_storage);
      continue;
    }
    if (split && sparse) {
      RETURN_IF_ERROR(reader.ReadEmpty());
      ASSIGN_OR_RETURN(BitVector validity, ReadBitmap(&reader, batch.rows));
      auto valid = static_cast<uint32_t>(validity.CountSetBits());
      FieldNode values_node = batch.nodes[serialized_column++];
      if (node.null_count != 0 ||
          values_node.null_count != batch.rows - valid) {
        return InvalidFile();
      }
      // The values' validity is implied: the first |valid| rows are set.
      RETURN_IF_ERROR(reader.Skip());
      if (column.storage.type().Is<core::String>()) {
        RETURN_IF_ERROR(ReadPackedDictionaryIndices(
            &reader, batch.rows, valid, dictionaries[dictionary_index++],
            &column.storage));
      } else {
        uint32_t width = NumericSize(column.storage.type());
        base::Status status;
        VisitNumericStorage(&column.storage, [&](auto* output) {
          output->resize(valid);
          status = reader.ReadInto(reinterpret_cast<uint8_t*>(output->data()),
                                   static_cast<uint64_t>(batch.rows) * width,
                                   static_cast<size_t>(valid) * width);
        });
        RETURN_IF_ERROR(status);
      }
      InstallValidity(nullability, std::move(validity), &column.null_storage);
      continue;
    }
    ASSIGN_OR_RETURN(
        BitVector validity,
        ReadValidityBuffer(&reader, batch.rows, node.null_count, nullable));
    uint32_t stored_rows =
        sparse ? batch.rows - static_cast<uint32_t>(node.null_count)
               : batch.rows;
    if (column.storage.type().Is<core::String>()) {
      RETURN_IF_ERROR(ReadDictionaryIndices(
          &reader, batch.rows, stored_rows, nullable, sparse, validity,
          dictionaries[dictionary_index++], &column.storage));
    } else {
      RETURN_IF_ERROR(ReadNumericBuffer(&reader, batch.rows, stored_rows,
                                        sparse, validity, &column.storage));
    }
    InstallValidity(nullability, std::move(validity), &column.null_storage);
  }
  dataframe.row_count_ = batch.rows;
  ++dataframe.non_column_mutations_;
  dataframe.Finalize();
  return dataframe;
#else   // PERFETTO_IS_LITTLE_ENDIAN()
  return base::ErrStatus(
      "Arrow deserialization is not supported on big-endian hosts");
#endif  // PERFETTO_IS_LITTLE_ENDIAN()
}

}  // namespace perfetto::trace_processor::core::dataframe
