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

#include "src/trace_processor/core/dataframe/arrow_ipc.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/util/flatbuf_reader.h"
#include "src/trace_processor/util/flatbuf_writer.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::core::dataframe {

using util::FlatBufferReader;
using util::FlatBufferWriter;
using W = FlatBufferWriter;

namespace {

// Arrow IPC file format constants.
constexpr uint8_t kMagicPadded[] = {'A', 'R', 'R', 'O', 'W', '1', 0, 0};
constexpr uint8_t kMagicTrailer[] = {'A', 'R', 'R', 'O', 'W', '1'};
constexpr uint32_t kContinuation = 0xFFFFFFFF;

// Arrow flatbuffer enum values.
constexpr int16_t kMetadataV5 = 4;
constexpr uint8_t kHeaderSchema = 1;
constexpr uint8_t kHeaderRecordBatch = 3;
constexpr uint8_t kTypeInt = 2;
constexpr uint8_t kTypeFloatingPoint = 3;
constexpr uint8_t kTypeUtf8 = 5;

uint32_t PadTo8(uint32_t n) {
  return (n + 7) & ~uint32_t{7};
}

// FieldNode struct: i64 length + i64 null_count = 16 bytes.
struct FieldNode {
  int64_t length;
  int64_t null_count;
};

// Buffer struct: i64 offset + i64 length = 16 bytes.
struct Buffer {
  int64_t offset;
  int64_t length;
};

// Block struct for Footer: i64 offset + i32 metaDataLength + i64 bodyLength.
// FlatBuffer structs use natural alignment, so there's 4 bytes padding.
#pragma pack(push, 1)
struct Block {
  int64_t offset;
  int32_t metadata_length;
  int32_t padding;
  int64_t body_length;
};
#pragma pack(pop)
static_assert(sizeof(Block) == 24, "Block struct must be 24 bytes");

// ---------------------------------------------------------------------------
// Flatbuffer builders for Arrow schema types.
// ---------------------------------------------------------------------------

// Int type table: field 0 = bitWidth (i32), field 1 = is_signed (bool).
W::Offset BuildIntType(W& w, int32_t bit_width, bool is_signed) {
  w.StartTable();
  w.FieldI32(0, bit_width);
  w.FieldBool(1, is_signed);
  return w.EndTable();
}

// FloatingPoint type table: field 0 = precision (i16).
// Precision: 0=HALF, 1=SINGLE, 2=DOUBLE.
W::Offset BuildFloatingPointType(W& w, int16_t precision) {
  w.StartTable();
  w.FieldI16(0, precision);
  return w.EndTable();
}

// Utf8 type table: empty table (no fields).
W::Offset BuildUtf8Type(W& w) {
  w.StartTable();
  return w.EndTable();
}

// Field table: field 0 = name, field 1 = nullable, field 2 = type_type,
//              field 3 = type.
W::Offset BuildField(W& w,
                      const std::string& name,
                      uint8_t type_type,
                      W::Offset type_off,
                      bool nullable) {
  auto name_off = w.WriteString(name);
  w.StartTable();
  w.FieldOffset(0, name_off);
  w.FieldBool(1, nullable);
  w.FieldU8(2, type_type);
  w.FieldOffset(3, type_off);
  return w.EndTable();
}

// Schema table: field 0 = endianness (i16), field 1 = fields (vec).
W::Offset BuildSchema(W& w,
                       const W::Offset* field_offsets,
                       uint32_t num_fields) {
  auto fields_vec = w.WriteVecOffsets(field_offsets, num_fields);
  w.StartTable();
  w.FieldI16(0, 0);  // little-endian
  w.FieldOffset(1, fields_vec);
  return w.EndTable();
}

// Build schema field offsets from a list of column infos, then build the
// Schema table. Avoids duplicating the field-building loop between the
// schema message and the footer.
W::Offset BuildSchemaWithFields(
    W& w,
    const std::vector<ArrowWriter::ColInfo>& cols) {
  std::vector<W::Offset> field_offsets;
  for (const auto& col : cols) {
    uint8_t type_type;
    W::Offset type_off;
    if (col.storage_type.Is<core::String>()) {
      type_type = kTypeUtf8;
      type_off = BuildUtf8Type(w);
    } else if (col.storage_type.Is<core::Double>()) {
      type_type = kTypeFloatingPoint;
      type_off = BuildFloatingPointType(w, 2);  // DOUBLE
    } else if (col.storage_type.Is<core::Int64>()) {
      type_type = kTypeInt;
      type_off = BuildIntType(w, 64, true);
    } else if (col.storage_type.Is<core::Int32>()) {
      type_type = kTypeInt;
      type_off = BuildIntType(w, 32, true);
    } else {
      // Uint32.
      type_type = kTypeInt;
      type_off = BuildIntType(w, 32, false);
    }
    field_offsets.push_back(
        BuildField(w, col.name, type_type, type_off, col.nullable));
  }
  return BuildSchema(w, field_offsets.data(),
                     static_cast<uint32_t>(field_offsets.size()));
}


// Creates a BitVector from Arrow validity bitmap bytes.
BitVector ReadValidityBitmap(const uint8_t* data, uint32_t num_rows) {
  auto bv = BitVector::CreateWithSize(num_rows, false);
  for (uint32_t i = 0; i < num_rows; i++) {
    if (data[i / 8] & (1u << (i % 8))) {
      bv.set(i);
    }
  }
  return bv;
}

// Returns true if the given nullability represents a nullable column.
bool IsNullable(Nullability n) {
  return !n.Is<core::NonNull>();
}

// Returns true if nulls are sparse (storage only holds non-null values).
bool IsSparseNull(Nullability n) {
  return !n.Is<core::NonNull>() && !n.Is<core::DenseNull>();
}

// Returns the element size in bytes for a numeric storage type.
uint32_t NumericElemSize(StorageType t) {
  if (t.Is<core::Int64>() || t.Is<core::Double>())
    return 8;
  return 4;  // Uint32 or Int32
}

// Returns a raw byte pointer to the numeric storage data.
const uint8_t* NumericRawData(const Storage& s) {
  if (s.type().Is<core::Double>())
    return reinterpret_cast<const uint8_t*>(s.unchecked_data<Double>());
  if (s.type().Is<core::Int64>())
    return reinterpret_cast<const uint8_t*>(s.unchecked_data<Int64>());
  if (s.type().Is<core::Int32>())
    return reinterpret_cast<const uint8_t*>(s.unchecked_data<Int32>());
  return reinterpret_cast<const uint8_t*>(s.unchecked_data<Uint32>());
}


// After data has been loaded, sets the validity BitVector on the column's
// NullStorage and computes prefix popcount for sparse-null columns.
void ApplyNullBitVector(Column& column,
                        Nullability nullability,
                        BitVector validity_bv) {
  if (nullability.Is<core::DenseNull>()) {
    column.null_storage.unchecked_get<core::DenseNull>().bit_vector =
        std::move(validity_bv);
  } else if (IsSparseNull(nullability)) {
    auto& sparse = column.null_storage.unchecked_get<core::SparseNull>();
    sparse.bit_vector = std::move(validity_bv);
    if (nullability.Is<core::SparseNullWithPopcountAlways>() ||
        nullability.Is<core::SparseNullWithPopcountUntilFinalization>()) {
      sparse.prefix_popcount_for_cell_get =
          sparse.bit_vector.PrefixPopcountFlexVector();
    }
  }
}

// Message table: field 0 = version (i16), field 1 = header_type (u8),
//                field 2 = header (offset), field 3 = bodyLength (i64).
W::Offset BuildMessage(W& w,
                        uint8_t header_type,
                        W::Offset header,
                        int64_t body_length) {
  w.StartTable();
  w.FieldI16(0, kMetadataV5);
  w.FieldU8(1, header_type);
  w.FieldOffset(2, header);
  w.FieldI64(3, body_length);
  return w.EndTable();
}

// RecordBatch table: field 0 = length (i64), field 1 = nodes (vec of struct),
//                    field 2 = buffers (vec of struct).
W::Offset BuildRecordBatch(W& w,
                            int64_t length,
                            const FieldNode* nodes,
                            uint32_t num_nodes,
                            const Buffer* buffers,
                            uint32_t num_buffers) {
  auto bufs = w.WriteVecStruct(buffers, sizeof(Buffer), num_buffers,
                                alignof(int64_t));
  auto nds =
      w.WriteVecStruct(nodes, sizeof(FieldNode), num_nodes, alignof(int64_t));
  w.StartTable();
  w.FieldI64(0, length);
  w.FieldOffset(1, nds);
  w.FieldOffset(2, bufs);
  return w.EndTable();
}

// Footer table: field 0 = version (i16), field 1 = schema (offset),
//               field 2 = dictionaries (vec), field 3 = recordBatches (vec).
W::Offset BuildFooter(W& w,
                       W::Offset schema,
                       const Block* batches,
                       uint32_t num_batches) {
  auto batches_vec =
      w.WriteVecStruct(batches, sizeof(Block), num_batches, alignof(int64_t));
  auto dicts_vec = w.WriteVecStruct(nullptr, sizeof(Block), 0, alignof(int64_t));
  w.StartTable();
  w.FieldI16(0, kMetadataV5);
  w.FieldOffset(1, schema);
  w.FieldOffset(2, dicts_vec);
  w.FieldOffset(3, batches_vec);
  return w.EndTable();
}

// Helper to append raw bytes to a vector.
void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  out.insert(out.end(), p, p + len);
}

// Helper to append N zero bytes to a vector.
void AppendZeros(std::vector<uint8_t>& out, size_t n) {
  out.resize(out.size() + n, 0);
}

// Helper to append a uint32_t in little-endian to a vector.
void AppendU32Le(std::vector<uint8_t>& out, uint32_t v) {
  AppendBytes(out, &v, 4);
}

}  // namespace

// ---------------------------------------------------------------------------
// ArrowWriter::Prepare — builds metadata, returns exact output size.
// ---------------------------------------------------------------------------
size_t ArrowWriter::Prepare(const Dataframe& df, StringPool* pool) {
  uint32_t num_cols = df.column_count();
  const auto& col_names = df.column_names();
  uint32_t num_rows = df.row_count();

  // Collect columns to serialize (skip Id columns).
  cols_.clear();
  for (uint32_t i = 0; i < num_cols; i++) {
    const auto& column = *df.columns_[i];
    if (column.storage.type().Is<core::Id>())
      continue;
    cols_.push_back({i, col_names[i],
                     IsNullable(column.null_storage.nullability()),
                     column.storage.type()});
  }

  // Pass 1: compute Buffer offsets/lengths, FieldNodes, string data sizes.
  std::vector<FieldNode> nodes;
  std::vector<Buffer> buffers;
  uint32_t body_cursor = 0;

  auto RecordBuffer = [&](uint32_t len) -> Buffer {
    int64_t off = static_cast<int64_t>(body_cursor);
    body_cursor += PadTo8(len);
    return {off, static_cast<int64_t>(len)};
  };

  string_data_lens_.assign(cols_.size(), 0);

  for (uint32_t col_i = 0; col_i < static_cast<uint32_t>(cols_.size());
       col_i++) {
    const auto& ci = cols_[col_i];
    const auto& column = *df.columns_[ci.idx];
    const auto& null_storage = column.null_storage;
    bool is_sparse = IsSparseNull(null_storage.nullability());

    int64_t null_count = 0;
    if (ci.nullable) {
      const BitVector& bv = null_storage.GetNullBitVector();
      for (uint32_t r = 0; r < num_rows; r++) {
        if (!bv.is_set(r))
          null_count++;
      }
      buffers.push_back(RecordBuffer((num_rows + 7) / 8));
    }

    if (ci.storage_type.Is<core::String>()) {
      buffers.push_back(RecordBuffer((num_rows + 1) * 4));
      const StringPool::Id* ids = column.storage.unchecked_data<String>();
      const BitVector* bv =
          ci.nullable ? &null_storage.GetNullBitVector() : nullptr;
      uint32_t sparse_idx = 0;
      uint32_t str_total = 0;
      for (uint32_t r = 0; r < num_rows; r++) {
        if (!bv || bv->is_set(r)) {
          str_total += static_cast<uint32_t>(
              pool->Get(ids[is_sparse ? sparse_idx++ : r]).size());
        }
      }
      string_data_lens_[col_i] = str_total;
      buffers.push_back(RecordBuffer(str_total));
    } else {
      buffers.push_back(
          RecordBuffer(num_rows * NumericElemSize(ci.storage_type)));
    }
    nodes.push_back({static_cast<int64_t>(num_rows), null_count});
  }

  body_size_ = body_cursor;
  padded_body_ = PadTo8(body_size_);

  // Build flatbuffer metadata (small).
  std::vector<uint8_t> schema_fb;
  {
    W fw;
    auto schema = BuildSchemaWithFields(fw, cols_);
    auto msg = BuildMessage(fw, kHeaderSchema, schema, 0);
    fw.Finish(msg);
    schema_fb.assign(fw.data(), fw.data() + fw.size());
  }

  std::vector<uint8_t> batch_fb;
  {
    W fw;
    auto rb = BuildRecordBatch(fw, static_cast<int64_t>(num_rows), nodes.data(),
                                static_cast<uint32_t>(nodes.size()),
                                buffers.data(),
                                static_cast<uint32_t>(buffers.size()));
    auto msg = BuildMessage(fw, kHeaderRecordBatch, rb,
                            static_cast<int64_t>(padded_body_));
    fw.Finish(msg);
    batch_fb.assign(fw.data(), fw.data() + fw.size());
  }

  uint32_t padded_schema = PadTo8(static_cast<uint32_t>(schema_fb.size()));
  uint32_t padded_batch_meta = PadTo8(static_cast<uint32_t>(batch_fb.size()));
  uint32_t batch_offset = 8 + 8 + padded_schema;

  std::vector<uint8_t> footer_fb;
  {
    W fw;
    auto schema = BuildSchemaWithFields(fw, cols_);
    Block batch_block;
    batch_block.offset = static_cast<int64_t>(batch_offset);
    batch_block.metadata_length =
        static_cast<int32_t>(8 + padded_batch_meta);
    batch_block.padding = 0;
    batch_block.body_length = static_cast<int64_t>(padded_body_);
    auto footer = BuildFooter(fw, schema, &batch_block, 1);
    fw.Finish(footer);
    footer_fb.assign(fw.data(), fw.data() + fw.size());
  }

  // Build header_ (magic + schema msg + record batch metadata).
  header_.clear();
  header_.reserve(8 + 8 + padded_schema + 8 + padded_batch_meta);
  AppendBytes(header_, kMagicPadded, 8);
  AppendU32Le(header_, kContinuation);
  AppendU32Le(header_, padded_schema);
  AppendBytes(header_, schema_fb.data(), schema_fb.size());
  AppendZeros(header_, padded_schema - schema_fb.size());
  AppendU32Le(header_, kContinuation);
  AppendU32Le(header_, padded_batch_meta);
  AppendBytes(header_, batch_fb.data(), batch_fb.size());
  AppendZeros(header_, padded_batch_meta - batch_fb.size());

  // Build trailer_ (footer + footer_size + magic).
  trailer_.clear();
  trailer_.reserve(footer_fb.size() + 10);
  AppendBytes(trailer_, footer_fb.data(), footer_fb.size());
  AppendU32Le(trailer_, static_cast<uint32_t>(footer_fb.size()));
  AppendBytes(trailer_, kMagicTrailer, 6);

  return header_.size() + padded_body_ + trailer_.size();
}

// ---------------------------------------------------------------------------
// ArrowWriter::Write — streams column data to sink.
// ---------------------------------------------------------------------------
base::Status ArrowWriter::Write(const Dataframe& df,
                                StringPool* pool,
                                const ArrowIpcWriteSink& sink) {
  uint32_t num_rows = df.row_count();

  sink(header_.data(), header_.size());

  // Write column data. O(columns) sink calls.
  for (uint32_t col_i = 0; col_i < static_cast<uint32_t>(cols_.size());
       col_i++) {
    const auto& ci = cols_[col_i];
    const auto& column = *df.columns_[ci.idx];
    const auto& storage = column.storage;
    const auto& null_storage = column.null_storage;
    bool is_sparse = IsSparseNull(null_storage.nullability());

    const BitVector* bv = nullptr;
    if (ci.nullable) {
      bv = &null_storage.GetNullBitVector();
      uint32_t bitmap_bytes = (num_rows + 7) / 8;
      uint32_t padded = PadTo8(bitmap_bytes);
      scratch_.resize(padded);
      // Build validity bitmap byte-at-a-time to avoid zeroing.
      uint32_t full_bytes = num_rows / 8;
      for (uint32_t b = 0; b < full_bytes; b++) {
        uint8_t byte = 0;
        uint32_t base = b * 8;
        for (uint32_t bit = 0; bit < 8; bit++) {
          if (bv->is_set(base + bit))
            byte |= static_cast<uint8_t>(1u << bit);
        }
        scratch_[b] = byte;
      }
      // Handle remaining bits in the last partial byte.
      uint32_t remaining = num_rows % 8;
      if (remaining) {
        uint8_t byte = 0;
        uint32_t base = full_bytes * 8;
        for (uint32_t bit = 0; bit < remaining; bit++) {
          if (bv->is_set(base + bit))
            byte |= static_cast<uint8_t>(1u << bit);
        }
        scratch_[full_bytes] = byte;
      }
      // Zero only the padding bytes.
      memset(scratch_.data() + bitmap_bytes, 0, padded - bitmap_bytes);
      sink(scratch_.data(), padded);
    }

    if (ci.storage_type.Is<core::String>()) {
      const StringPool::Id* ids = storage.unchecked_data<String>();
      uint32_t offsets_bytes = (num_rows + 1) * 4;
      uint32_t padded_offsets = PadTo8(offsets_bytes);
      uint32_t str_len = string_data_lens_[col_i];
      uint32_t padded_str = PadTo8(str_len);
      uint32_t total = padded_offsets + padded_str;

      scratch_.resize(total);
      auto* offsets = reinterpret_cast<int32_t*>(scratch_.data());
      uint8_t* str_dst = scratch_.data() + padded_offsets;

      int32_t current_offset = 0;
      uint32_t sparse_idx = 0;
      for (uint32_t r = 0; r < num_rows; r++) {
        offsets[r] = current_offset;
        if (!bv || bv->is_set(r)) {
          NullTermStringView sv =
              pool->Get(ids[is_sparse ? sparse_idx++ : r]);
          auto len = static_cast<int32_t>(sv.size());
          if (len > 0) {
            memcpy(str_dst + current_offset, sv.data(),
                   static_cast<size_t>(len));
          }
          current_offset += len;
        }
      }
      offsets[num_rows] = current_offset;
      // Zero only the padding bytes between offsets and string data,
      // and after string data.
      memset(scratch_.data() + offsets_bytes, 0, padded_offsets - offsets_bytes);
      memset(str_dst + str_len, 0, padded_str - str_len);
      sink(scratch_.data(), total);
    } else {
      uint32_t elem_size = NumericElemSize(ci.storage_type);
      uint32_t data_len = num_rows * elem_size;
      uint32_t padded_data = PadTo8(data_len);
      const uint8_t* raw = NumericRawData(storage);

      if (!is_sparse) {
        sink(raw, data_len);
        if (padded_data > data_len) {
          uint8_t zeros[8] = {};
          sink(zeros, padded_data - data_len);
        }
      } else {
        scratch_.resize(padded_data);
        uint32_t si = 0;
        for (uint32_t r = 0; r < num_rows; r++) {
          size_t dst_off = static_cast<size_t>(r) * elem_size;
          if (bv->is_set(r)) {
            memcpy(scratch_.data() + dst_off,
                   raw + static_cast<size_t>(si) * elem_size, elem_size);
            si++;
          } else {
            memset(scratch_.data() + dst_off, 0, elem_size);
          }
        }
        // Zero only padding.
        memset(scratch_.data() + data_len, 0, padded_data - data_len);
        sink(scratch_.data(), padded_data);
      }
    }
  }

  if (padded_body_ > body_size_) {
    uint8_t zeros[8] = {};
    sink(zeros, padded_body_ - body_size_);
  }

  sink(trailer_.data(), trailer_.size());
  return base::OkStatus();
}

base::Status DeserializeFromArrowIpc(
    Dataframe& df,
    StringPool* pool,
    const util::TraceBlobViewReader& reader) {
  size_t len = reader.end_offset() - reader.start_offset();

  // Minimum: 8 (magic) + 4 (footer_size) + 6 (magic) = 18.
  if (len < 18)
    return base::ErrStatus("Arrow IPC file too small");

  // Check header magic.
  auto header = reader.SliceOff(reader.start_offset(), 8);
  if (!header)
    return base::ErrStatus("Cannot read Arrow IPC header");
  if (memcmp(header->data(), "ARROW1", 6) != 0)
    return base::ErrStatus("Missing Arrow IPC header magic");

  // Check trailer magic.
  size_t trailer_off = reader.end_offset() - 6;
  auto trailer = reader.SliceOff(trailer_off, 6);
  if (!trailer)
    return base::ErrStatus("Cannot read Arrow IPC trailer");
  if (memcmp(trailer->data(), "ARROW1", 6) != 0)
    return base::ErrStatus("Missing Arrow IPC trailer magic");

  // Read footer size.
  auto footer_size_blob = reader.SliceOff(reader.end_offset() - 10, 4);
  if (!footer_size_blob)
    return base::ErrStatus("Cannot read Arrow IPC footer size");
  uint32_t footer_size;
  memcpy(&footer_size, footer_size_blob->data(), 4);
  if (footer_size + 18 > len)
    return base::ErrStatus("Arrow IPC footer size exceeds file size");

  // Read and parse footer flatbuffer.
  size_t footer_off = reader.end_offset() - 10 - footer_size;
  auto footer_blob = reader.SliceOff(footer_off, footer_size);
  if (!footer_blob)
    return base::ErrStatus("Cannot read Arrow IPC footer");

  auto footer = FlatBufferReader::GetRoot(footer_blob->data(), footer_size);
  if (!footer)
    return base::ErrStatus("Failed to parse Arrow IPC footer");

  // Footer field 1 = schema.
  auto schema = footer->Table(1);
  if (!schema)
    return base::ErrStatus("Footer missing schema");

  // Footer field 3 = recordBatches (vector of Block structs).
  auto blocks = footer->VecScalar<Block>(3);
  if (blocks.size() == 0)
    return base::ErrStatus("No record batches in file");

  Block block = blocks[0];

  // Read the RecordBatch message at the block offset.
  auto msg_offset = static_cast<size_t>(block.offset);
  if (msg_offset + 8 > reader.end_offset())
    return base::ErrStatus("RecordBatch message offset out of bounds");

  // Read continuation marker (4 bytes) + metadata length (4 bytes).
  auto msg_header = reader.SliceOff(msg_offset, 8);
  if (!msg_header)
    return base::ErrStatus("Cannot read RecordBatch message header");

  uint32_t cont;
  memcpy(&cont, msg_header->data(), 4);
  if (cont != kContinuation)
    return base::ErrStatus("Missing continuation marker");

  int32_t meta_len;
  memcpy(&meta_len, msg_header->data() + 4, 4);
  uint32_t padded_meta = static_cast<uint32_t>(meta_len);

  // Read the message metadata.
  auto meta_blob = reader.SliceOff(msg_offset + 8, padded_meta);
  if (!meta_blob)
    return base::ErrStatus("Cannot read RecordBatch metadata");

  auto msg = FlatBufferReader::GetRoot(meta_blob->data(), padded_meta);
  if (!msg)
    return base::ErrStatus("Failed to parse RecordBatch message");

  // Message field 2 = header (RecordBatch table).
  auto rb = msg->Table(2);
  if (!rb)
    return base::ErrStatus("Message missing RecordBatch header");

  // RecordBatch field 0 = length (i64).
  int64_t num_rows = rb.Scalar<int64_t>(0);

  // RecordBatch field 2 = buffers (vector of Buffer structs).
  auto bufs = rb.VecScalar<Buffer>(2);

  // Body data starts after the message header.
  size_t body_offset = msg_offset + 8 + padded_meta;

  // Map Arrow columns to dataframe columns (skip Id columns — just set the
  // row count since their value is the row index).
  uint32_t buf_idx = 0;
  for (uint32_t i = 0; i < df.column_count(); i++) {
    if (df.columns_[i]->storage.type().Is<core::Id>()) {
      df.columns_[i]->storage.unchecked_get<Id>().size =
          static_cast<uint32_t>(num_rows);
      continue;
    }

    auto& column = *df.columns_[i];
    Nullability nullability = column.null_storage.nullability();
    bool nullable = IsNullable(nullability);

    // Read validity bitmap for nullable columns.
    BitVector validity_bv;
    if (nullable) {
      if (buf_idx >= bufs.size())
        return base::ErrStatus("Not enough buffers for column %u", i);
      Buffer bitmap_buf = bufs[buf_idx++];
      auto bitmap_data = reader.SliceOff(
          body_offset + static_cast<size_t>(bitmap_buf.offset),
          static_cast<size_t>(bitmap_buf.length));
      if (!bitmap_data)
        return base::ErrStatus("Cannot read validity bitmap for column %u", i);
      validity_bv = ReadValidityBitmap(bitmap_data->data(),
                                       static_cast<uint32_t>(num_rows));
    }

    auto& storage = column.storage;
    bool is_sparse = IsSparseNull(nullability);
    auto nr = static_cast<uint32_t>(num_rows);

    // Helper to consume the next buffer from the record batch.
    auto read_buf = [&]() -> base::StatusOr<TraceBlobView> {
      if (buf_idx >= bufs.size())
        return base::ErrStatus("Not enough buffers for column %u", i);
      Buffer b = bufs[buf_idx++];
      auto blob = reader.SliceOff(
          body_offset + static_cast<size_t>(b.offset),
          static_cast<size_t>(b.length));
      if (!blob)
        return base::ErrStatus("Cannot read buffer for column %u", i);
      return std::move(*blob);
    };

    if (storage.type().Is<core::String>()) {
      // String column: read offsets buffer then data buffer.
      auto offsets_blob = read_buf();
      if (!offsets_blob.ok())
        return offsets_blob.status();
      auto str_blob = read_buf();
      if (!str_blob.ok())
        return str_blob.status();

      const auto* offsets =
          reinterpret_cast<const int32_t*>(offsets_blob->data());
      const auto* chars = reinterpret_cast<const char*>(str_blob->data());
      auto& vec = storage.unchecked_get<String>();

      for (uint32_t r = 0; r < nr; r++) {
        bool valid = !nullable || validity_bv.is_set(r);
        if (valid) {
          vec.push_back(pool->InternString(base::StringView(
              chars + offsets[r],
              static_cast<size_t>(offsets[r + 1] - offsets[r]))));
        } else if (!is_sparse) {
          // DenseNull: placeholder for null slot.
          vec.push_back(StringPool::Id::Null());
        }
      }
    } else {
      // Numeric column (Uint32 or Double): read data buffer.
      auto data_blob = read_buf();
      if (!data_blob.ok())
        return data_blob.status();
      // Helper to deserialize a numeric column into its FlexVector.
      auto deser_numeric = [&](auto& vec) {
        using T = std::remove_reference_t<decltype(*vec.data())>;
        const auto* src = reinterpret_cast<const T*>(data_blob->data());
        if (!is_sparse) {
          vec.resize(static_cast<uint64_t>(num_rows));
          memcpy(vec.data(), src, nr * sizeof(T));
        } else {
          for (uint32_t r = 0; r < nr; r++) {
            if (validity_bv.is_set(r))
              vec.push_back(src[r]);
          }
        }
      };
      if (storage.type().Is<core::Double>()) {
        deser_numeric(storage.unchecked_get<Double>());
      } else if (storage.type().Is<core::Int64>()) {
        deser_numeric(storage.unchecked_get<Int64>());
      } else if (storage.type().Is<core::Int32>()) {
        deser_numeric(storage.unchecked_get<Int32>());
      } else {
        deser_numeric(storage.unchecked_get<Uint32>());
      }
    }

    // Apply validity bitmap to null storage (no-op for NonNull).
    ApplyNullBitVector(column, nullability, std::move(validity_bv));
  }

  df.row_count_ = static_cast<uint32_t>(num_rows);
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::core::dataframe
