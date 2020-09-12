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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_TRACE_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_TRACE_TOKENIZER_H_

#include <vector>

#include "perfetto/protozero/proto_utils.h"
#include "perfetto/trace_processor/status.h"
#include "src/trace_processor/importers/gzip/gzip_utils.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/util/status_macros.h"

#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace trace_processor {

// Reads a protobuf trace in chunks and extracts boundaries of trace packets
// (or subfields, for the case of ftrace) with their timestamps.
class ProtoTraceTokenizer {
 public:
  ProtoTraceTokenizer();

  template <typename Callback = util::Status(TraceBlobView)>
  util::Status Tokenize(std::unique_ptr<uint8_t[]> owned_buf,
                        size_t size,
                        Callback callback) {
    uint8_t* data = &owned_buf[0];
    if (!partial_buf_.empty()) {
      // It takes ~5 bytes for a proto preamble + the varint size.
      const size_t kHeaderBytes = 5;
      if (PERFETTO_UNLIKELY(partial_buf_.size() < kHeaderBytes)) {
        size_t missing_len = std::min(kHeaderBytes - partial_buf_.size(), size);
        partial_buf_.insert(partial_buf_.end(), &data[0], &data[missing_len]);
        if (partial_buf_.size() < kHeaderBytes)
          return util::OkStatus();
        data += missing_len;
        size -= missing_len;
      }

      // At this point we have enough data in |partial_buf_| to read at least
      // the field header and know the size of the next TracePacket.
      const uint8_t* pos = &partial_buf_[0];
      uint8_t proto_field_tag = *pos;
      uint64_t field_size = 0;
      const uint8_t* next = protozero::proto_utils::ParseVarInt(
          ++pos, &*partial_buf_.end(), &field_size);
      bool parse_failed = next == pos;
      pos = next;
      if (proto_field_tag != kTracePacketTag || field_size == 0 ||
          parse_failed) {
        return util::ErrStatus(
            "Failed parsing a TracePacket from the partial buffer");
      }

      // At this point we know how big the TracePacket is.
      size_t hdr_size = static_cast<size_t>(pos - &partial_buf_[0]);
      size_t size_incl_header = static_cast<size_t>(field_size + hdr_size);
      PERFETTO_DCHECK(size_incl_header > partial_buf_.size());

      // There is a good chance that between the |partial_buf_| and the new
      // |data| of the current call we have enough bytes to parse a TracePacket.
      if (partial_buf_.size() + size >= size_incl_header) {
        // Create a new buffer for the whole TracePacket and copy into that:
        // 1) The beginning of the TracePacket (including the proto header) from
        //    the partial buffer.
        // 2) The rest of the TracePacket from the current |data| buffer (note
        //    that we might have consumed already a few bytes form |data|
        //    earlier in this function, hence we need to keep |off| into
        //    account).
        std::unique_ptr<uint8_t[]> buf(new uint8_t[size_incl_header]);
        memcpy(&buf[0], partial_buf_.data(), partial_buf_.size());
        // |size_missing| is the number of bytes for the rest of the TracePacket
        // in |data|.
        size_t size_missing = size_incl_header - partial_buf_.size();
        memcpy(&buf[partial_buf_.size()], &data[0], size_missing);
        data += size_missing;
        size -= size_missing;
        partial_buf_.clear();
        uint8_t* buf_start = &buf[0];  // Note that buf is std::moved below.
        RETURN_IF_ERROR(ParseInternal(std::move(buf), buf_start,
                                      size_incl_header, callback));
      } else {
        partial_buf_.insert(partial_buf_.end(), data, &data[size]);
        return util::OkStatus();
      }
    }
    return ParseInternal(std::move(owned_buf), data, size, callback);
  }

 private:
  static constexpr uint8_t kTracePacketTag =
      protozero::proto_utils::MakeTagLengthDelimited(
          protos::pbzero::Trace::kPacketFieldNumber);

  template <typename Callback = util::Status(TraceBlobView)>
  util::Status ParseInternal(std::unique_ptr<uint8_t[]> owned_buf,
                             uint8_t* data,
                             size_t size,
                             Callback callback) {
    PERFETTO_DCHECK(data >= &owned_buf[0]);
    const uint8_t* start = &owned_buf[0];
    const size_t data_off = static_cast<size_t>(data - start);
    TraceBlobView whole_buf(std::move(owned_buf), data_off, size);

    protos::pbzero::Trace::Decoder decoder(data, size);
    for (auto it = decoder.packet(); it; ++it) {
      protozero::ConstBytes packet = *it;
      size_t field_offset = whole_buf.offset_of(packet.data);
      TraceBlobView sliced = whole_buf.slice(field_offset, packet.size);
      RETURN_IF_ERROR(ParsePacket(std::move(sliced), callback));
    }

    const size_t bytes_left = decoder.bytes_left();
    if (bytes_left > 0) {
      PERFETTO_DCHECK(partial_buf_.empty());
      partial_buf_.insert(partial_buf_.end(), &data[decoder.read_offset()],
                          &data[decoder.read_offset() + bytes_left]);
    }
    return util::OkStatus();
  }

  template <typename Callback = util::Status(TraceBlobView)>
  util::Status ParsePacket(TraceBlobView packet, Callback callback) {
    protos::pbzero::TracePacket::Decoder decoder(packet.data(),
                                                 packet.length());
    if (decoder.has_compressed_packets()) {
      if (!gzip::IsGzipSupported()) {
        return util::Status(
            "Cannot decode compressed packets. Zlib not enabled");
      }

      protozero::ConstBytes field = decoder.compressed_packets();
      const size_t field_off = packet.offset_of(field.data);
      TraceBlobView compressed_packets = packet.slice(field_off, field.size);
      TraceBlobView packets(nullptr, 0, 0);

      RETURN_IF_ERROR(Decompress(std::move(compressed_packets), &packets));

      const uint8_t* start = packets.data();
      const uint8_t* end = packets.data() + packets.length();
      const uint8_t* ptr = start;
      while ((end - ptr) > 2) {
        const uint8_t* packet_start = ptr;
        if (PERFETTO_UNLIKELY(*ptr != kTracePacketTag))
          return util::ErrStatus("Expected TracePacket tag");
        uint64_t packet_size = 0;
        ptr = protozero::proto_utils::ParseVarInt(++ptr, end, &packet_size);
        size_t packet_offset = static_cast<size_t>(ptr - start);
        ptr += packet_size;
        if (PERFETTO_UNLIKELY((ptr - packet_start) < 2 || ptr > end))
          return util::ErrStatus("Invalid packet size");

        TraceBlobView sliced =
            packets.slice(packet_offset, static_cast<size_t>(packet_size));
        RETURN_IF_ERROR(ParsePacket(std::move(sliced), callback));
      }
      return util::OkStatus();
    }
    return callback(std::move(packet));
  }

  util::Status Decompress(TraceBlobView input, TraceBlobView* output);

  // Used to glue together trace packets that span across two (or more)
  // Parse() boundaries.
  std::vector<uint8_t> partial_buf_;

  // Allows support for compressed trace packets.
  GzipDecompressor decompressor_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROTO_TRACE_TOKENIZER_H_
