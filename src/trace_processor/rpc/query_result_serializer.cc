/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/rpc/query_result_serializer.h"

#include <vector>

#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/proto_utils.h"
#include "protos/perfetto/trace_processor/trace_processor.pbzero.h"
#include "src/trace_processor/iterator_impl.h"

namespace perfetto {
namespace trace_processor {

namespace {

namespace pu = ::protozero::proto_utils;
using BatchProto = protos::pbzero::QueryResult::CellsBatch;
using ResultProto = protos::pbzero::QueryResult;

// The reserved field in trace_processor.proto.
static constexpr uint32_t kPaddingFieldId = 7;

uint8_t MakeLenDelimTag(uint32_t field_num) {
  uint32_t tag = pu::MakeTagLengthDelimited(field_num);
  PERFETTO_DCHECK(tag <= 127);  // Must fit in one byte.
  return static_cast<uint8_t>(tag);
}

}  // namespace

QueryResultSerializer::QueryResultSerializer(Iterator iter)
    : iter_(iter.take_impl()), num_cols_(iter_->ColumnCount()) {}

QueryResultSerializer::~QueryResultSerializer() = default;

bool QueryResultSerializer::Serialize(std::vector<uint8_t>* buf) {
  PERFETTO_CHECK(!eof_reached_);

  // In non-production builds avoid the big reservation. This is to avoid hiding
  // bugs that accidentally depend on pointer stability across resizes.
#if !PERFETTO_DCHECK_IS_ON()
  buf->reserve(buf->size() + batch_split_threshold_ + 4096);
#endif

  if (!did_write_column_names_) {
    SerializeColumnNames(buf);
    did_write_column_names_ = true;
  }

  // In case of an error we still want to go through SerializeBatch(). That will
  // write an empty batch with the EOF marker. Errors can happen also in the
  // middle of a query, not just before starting it.

  SerializeBatch(buf);
  MaybeSerializeError(buf);

  return !eof_reached_;
}

void QueryResultSerializer::SerializeBatch(std::vector<uint8_t>* buf) {
  // The buffer is filled in this way:
  // - Append all the strings as we iterate through the results. The rationale
  //   is that strings are typically the largest part of the result and we want
  //   to avoid copying these.
  // - While iterating, buffer all other types of cells. They will be appended
  //   at the end of the batch, after the string payload is known.

  // Note: this function uses uint32_t instead of size_t because Wasm doesn't
  // have yet native 64-bit integers and this is perf-sensitive.
  const uint32_t initial_size = static_cast<uint32_t>(buf->size());

  buf->push_back(MakeLenDelimTag(ResultProto::kBatchFieldNumber));
  const uint32_t batch_size_hdr = static_cast<uint32_t>(buf->size());
  buf->resize(batch_size_hdr + pu::kMessageLengthFieldSize);

  // Start the |string_cells|.
  buf->push_back(MakeLenDelimTag(BatchProto::kStringCellsFieldNumber));
  const uint32_t strings_hdr_off = static_cast<uint32_t>(buf->size());
  buf->resize(strings_hdr_off + pu::kMessageLengthFieldSize);
  const uint32_t strings_start_off = static_cast<uint32_t>(buf->size());

  // This keeps track of the overall size of the batch. It is used to decide if
  // we need to prematurely end the batch, even if the batch_split_threshold_ is
  // not reached. This is to guard against the degenerate case of appending a
  // lot of very large strings and ending up with an enormous batch.
  auto approx_batch_size = static_cast<uint32_t>(buf->size()) - initial_size;

  std::vector<uint8_t> cell_types(cells_per_batch_);

  // Varints and doubles are written on stack-based storage and appended later.
  protozero::PackedVarInt varints;
  protozero::PackedFixedSizeInt<double> doubles;

  // We write blobs on a temporary heap buffer and append it at the end. Blobs
  // are extremely rare, trying to avoid copies is not worth the complexity.
  std::vector<uint8_t> blobs;

  uint32_t cell_idx = 0;
  bool batch_full = false;

  // Skip block if the query didn't return any result (e.g. CREATE TABLE).
  for (; num_cols_ > 0; ++cell_idx, ++col_) {
    // This branch is hit before starting each row. Note that iter_->Next() must
    // be called before iterating on a row. col_ is initialized at MAX_INT in
    // the constructor.
    if (col_ >= num_cols_) {
      col_ = 0;
      if (!iter_->Next())
        break;  // EOF or error.

      // We need to guarantee that a batch contains whole rows. Before moving to
      // the next row, make sure that: (i) there is space for all the columns;
      // (ii) the batch didn't grow too much.
      if (cell_idx + num_cols_ > cells_per_batch_ ||
          approx_batch_size > batch_split_threshold_) {
        batch_full = true;
        break;
      }
    }

    auto value = iter_->Get(col_);
    uint8_t cell_type = BatchProto::CELL_INVALID;
    switch (value.type) {
      case SqlValue::Type::kNull: {
        cell_type = BatchProto::CELL_NULL;
        break;
      }
      case SqlValue::Type::kLong: {
        cell_type = BatchProto::CELL_VARINT;
        varints.Append(value.long_value);
        approx_batch_size += 4;  // Just a guess, doesn't need to be accurate.
        break;
      }
      case SqlValue::Type::kDouble: {
        cell_type = BatchProto::CELL_FLOAT64;
        approx_batch_size += sizeof(double);
        doubles.Append(value.double_value);
        break;
      }
      case SqlValue::Type::kString: {
        // Append the string to the one |string_cells| proto field, just use
        // \0 to separate each string. We are deliberately NOT emitting one
        // proto repeated field for each string. Doing so significantly slows
        // down parsing on the JS side (go/postmessage-benchmark).
        cell_type = BatchProto::CELL_STRING;
        uint32_t len_with_nul =
            static_cast<uint32_t>(strlen(value.string_value)) + 1;
        const char* str_begin = value.string_value;
        buf->insert(buf->end(), str_begin, str_begin + len_with_nul);
        approx_batch_size += len_with_nul;
        break;
      }
      case SqlValue::Type::kBytes: {
        // Each blob is stored as its own repeated proto field, unlike strings.
        // Blobs don't incur in text-decoding overhead (and are also rare).
        cell_type = BatchProto::CELL_BLOB;
        auto* src = static_cast<const uint8_t*>(value.bytes_value);
        uint32_t len = static_cast<uint32_t>(value.bytes_count);
        uint8_t preamble[16];
        uint8_t* preamble_end = &preamble[0];
        *(preamble_end++) = MakeLenDelimTag(BatchProto::kBlobCellsFieldNumber);
        preamble_end = pu::WriteVarInt(len, preamble_end);
        blobs.insert(blobs.end(), preamble, preamble_end);
        blobs.insert(blobs.end(), src, src + len);
        approx_batch_size += len + 4;  // 4 is a guess on the preamble size.
        break;
      }
    }

    PERFETTO_DCHECK(cell_type != BatchProto::CELL_INVALID);
    cell_types[cell_idx] = cell_type;
  }  // for (cell)

  // Backfill the string size.
  auto strings_size = static_cast<uint32_t>(buf->size() - strings_start_off);
  pu::WriteRedundantVarInt(strings_size, buf->data() + strings_hdr_off);

  // Write the cells headers (1 byte per cell).
  {
    uint8_t preamble[16];
    uint8_t* preamble_end = &preamble[0];
    *(preamble_end++) = MakeLenDelimTag(BatchProto::kCellsFieldNumber);
    preamble_end = pu::WriteVarInt(cell_idx, preamble_end);
    buf->insert(buf->end(), preamble, preamble_end);
    buf->insert(buf->end(), cell_types.data(), cell_types.data() + cell_idx);
  }

  // Append the |varint_cells|, copying over the packed varint buffer.
  const uint32_t varints_size = static_cast<uint32_t>(varints.size());
  if (varints_size > 0) {
    uint8_t preamble[16];
    uint8_t* preamble_end = &preamble[0];
    *(preamble_end++) = MakeLenDelimTag(BatchProto::kVarintCellsFieldNumber);
    preamble_end = pu::WriteVarInt(varints_size, preamble_end);
    buf->insert(buf->end(), preamble, preamble_end);
    buf->insert(buf->end(), varints.data(), varints.data() + varints_size);
  }

  // Append the |float64_cells|, copying over the packed fixed64 buffer. This is
  // appended at a 64-bit aligned offset, so that JS can access these by overlay
  // a TypedArray, without extra copies.
  const uint32_t doubles_size = static_cast<uint32_t>(doubles.size());
  if (doubles_size > 0) {
    uint8_t preamble[16];
    uint8_t* preamble_end = &preamble[0];
    *(preamble_end++) = MakeLenDelimTag(BatchProto::kFloat64CellsFieldNumber);
    preamble_end = pu::WriteVarInt(doubles_size, preamble_end);
    uint32_t preamble_size = static_cast<uint32_t>(preamble_end - &preamble[0]);

    // The byte after the preamble must start at a 64bit-aligned offset.
    // The padding needs to be > 1 Byte because of proto encoding.
    const uint32_t off = static_cast<uint32_t>(buf->size() + preamble_size);
    const uint32_t aligned_off = (off + 7) & ~7u;
    uint32_t padding = aligned_off - off;
    if (padding == 1)
      padding = 9;

    if (padding > 0) {
      buf->push_back(pu::MakeTagVarInt(kPaddingFieldId));
      for (uint32_t i = 0; i < padding - 2; i++)
        buf->push_back(0x80);
      buf->push_back(0);
    }

    buf->insert(buf->end(), preamble, preamble_end);
    PERFETTO_CHECK(buf->size() % 8 == 0);
    buf->insert(buf->end(), doubles.data(), doubles.data() + doubles_size);
  }  // if (doubles_size > 0)

  // Append the blobs.
  buf->insert(buf->end(), blobs.begin(), blobs.end());

  // If this is the last batch, write the EOF field.
  if (!batch_full) {
    eof_reached_ = true;
    auto kEofTag = pu::MakeTagVarInt(BatchProto::kIsLastBatchFieldNumber);
    buf->push_back(static_cast<uint8_t>(kEofTag));
    buf->push_back(1);
  }

  // Finally backfill the size of the whole |batch| sub-message.
  const uint32_t batch_size = static_cast<uint32_t>(
      buf->size() - batch_size_hdr - pu::kMessageLengthFieldSize);
  pu::WriteRedundantVarInt(batch_size, buf->data() + batch_size_hdr);
}

void QueryResultSerializer::MaybeSerializeError(std::vector<uint8_t>* buf) {
  if (iter_->Status().ok())
    return;
  std::string err = iter_->Status().message();
  // Make sure the |error| field is always non-zero if the query failed, so
  // the client can tell some error happened.
  if (err.empty())
    err = "Unknown error";

  // Write the error and return.
  uint8_t preamble[16];
  uint8_t* preamble_end = &preamble[0];
  *(preamble_end++) = MakeLenDelimTag(ResultProto::kErrorFieldNumber);
  preamble_end = pu::WriteVarInt(err.size(), preamble_end);
  buf->insert(buf->end(), preamble, preamble_end);
  buf->insert(buf->end(), err.begin(), err.end());
}

void QueryResultSerializer::SerializeColumnNames(std::vector<uint8_t>* buf) {
  PERFETTO_DCHECK(!did_write_column_names_);
  for (uint32_t c = 0; c < num_cols_; c++) {
    std::string col_name = iter_->GetColumnName(c);
    uint8_t preamble[16];
    uint8_t* preamble_end = &preamble[0];
    *(preamble_end++) = MakeLenDelimTag(ResultProto::kColumnNamesFieldNumber);
    preamble_end = pu::WriteVarInt(col_name.size(), preamble_end);
    buf->insert(buf->end(), preamble, preamble_end);
    buf->insert(buf->end(), col_name.begin(), col_name.end());
  }
}

}  // namespace trace_processor
}  // namespace perfetto
