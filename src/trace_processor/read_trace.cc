/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/trace_processor/read_trace.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/trace_processor/trace_processor.h"

#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/importers/gzip/gzip_trace_parser.h"
#include "src/trace_processor/importers/proto/proto_trace_tokenizer.h"
#include "src/trace_processor/util/gzip_utils.h"
#include "src/trace_processor/util/status_macros.h"

#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#if TRACE_PROCESSOR_HAS_MMAP()
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace perfetto {
namespace trace_processor {
namespace {

// 1MB chunk size seems the best tradeoff on a MacBook Pro 2013 - i7 2.8 GHz.
constexpr size_t kChunkSize = 1024 * 1024;

util::Status ReadTraceUsingRead(
    TraceProcessor* tp,
    int fd,
    uint64_t* file_size,
    const std::function<void(uint64_t parsed_size)>& progress_callback) {
  // Load the trace in chunks using ordinary read().
  for (int i = 0;; i++) {
    if (progress_callback && i % 128 == 0)
      progress_callback(*file_size);

    TraceBlob blob = TraceBlob::Allocate(kChunkSize);
    auto rsize = base::Read(fd, blob.data(), blob.size());
    if (rsize == 0)
      break;

    if (rsize < 0) {
      return util::ErrStatus("Reading trace file failed (errno: %d, %s)", errno,
                             strerror(errno));
    }

    *file_size += static_cast<uint64_t>(rsize);
    TraceBlobView blob_view(std::move(blob), 0, static_cast<size_t>(rsize));
    RETURN_IF_ERROR(tp->Parse(std::move(blob_view)));
  }
  return util::OkStatus();
}

class SerializingProtoTraceReader : public ChunkedTraceReader {
 public:
  explicit SerializingProtoTraceReader(std::vector<uint8_t>* output)
      : output_(output) {}

  util::Status Parse(TraceBlobView blob) override {
    return tokenizer_.Tokenize(std::move(blob), [this](TraceBlobView packet) {
      uint8_t buffer[protozero::proto_utils::kMaxSimpleFieldEncodedSize];

      uint8_t* pos = buffer;
      pos = protozero::proto_utils::WriteVarInt(kTracePacketTag, pos);
      pos = protozero::proto_utils::WriteVarInt(packet.length(), pos);
      output_->insert(output_->end(), buffer, pos);

      output_->insert(output_->end(), packet.data(),
                      packet.data() + packet.length());
      return util::OkStatus();
    });
  }

  void NotifyEndOfFile() override {}

 private:
  static constexpr uint8_t kTracePacketTag =
      protozero::proto_utils::MakeTagLengthDelimited(
          protos::pbzero::Trace::kPacketFieldNumber);

  ProtoTraceTokenizer tokenizer_;
  std::vector<uint8_t>* output_;
};

}  // namespace

util::Status ReadTrace(
    TraceProcessor* tp,
    const char* filename,
    const std::function<void(uint64_t parsed_size)>& progress_callback) {
  base::ScopedFile fd(base::OpenFile(filename, O_RDONLY));
  if (!fd)
    return util::ErrStatus("Could not open trace file (path: %s)", filename);

  uint64_t bytes_read = 0;

#if TRACE_PROCESSOR_HAS_MMAP()
  char* no_mmap = getenv("TRACE_PROCESSOR_NO_MMAP");
  uint64_t whole_size_64 = static_cast<uint64_t>(lseek(*fd, 0, SEEK_END));
  lseek(*fd, 0, SEEK_SET);
  bool use_mmap = !no_mmap || *no_mmap != '1';
  if (sizeof(size_t) < 8 && whole_size_64 > 2147483648ULL)
    use_mmap = false;  // Cannot use mmap on 32-bit systems for files > 2GB.

  if (use_mmap) {
    const size_t whole_size = static_cast<size_t>(whole_size_64);
    void* file_mm = mmap(nullptr, whole_size, PROT_READ, MAP_PRIVATE, *fd, 0);
    if (file_mm != MAP_FAILED) {
      TraceBlobView whole_mmap(TraceBlob::FromMmap(file_mm, whole_size));
      // Parse the file in chunks so we get some status update on stdio.
      static constexpr size_t kMmapChunkSize = 128ul * 1024 * 1024;
      while (bytes_read < whole_size_64) {
        progress_callback(bytes_read);
        const size_t bytes_read_z = static_cast<size_t>(bytes_read);
        size_t slice_size = std::min(whole_size - bytes_read_z, kMmapChunkSize);
        TraceBlobView slice = whole_mmap.slice_off(bytes_read_z, slice_size);
        RETURN_IF_ERROR(tp->Parse(std::move(slice)));
        bytes_read += slice_size;
      }  // while (slices)
    }    // if (!MAP_FAILED)
  }      // if (use_mmap)
  if (bytes_read == 0)
    PERFETTO_LOG("Cannot use mmap on this system. Falling back on read()");
#endif  // TRACE_PROCESSOR_HAS_MMAP()
  if (bytes_read == 0) {
    RETURN_IF_ERROR(
        ReadTraceUsingRead(tp, *fd, &bytes_read, progress_callback));
  }
  tp->NotifyEndOfFile();
  tp->SetCurrentTraceName(filename);

  if (progress_callback)
    progress_callback(bytes_read);
  return util::OkStatus();
}

util::Status DecompressTrace(const uint8_t* data,
                             size_t size,
                             std::vector<uint8_t>* output) {
  TraceType type = GuessTraceType(data, size);
  if (type != TraceType::kGzipTraceType && type != TraceType::kProtoTraceType) {
    return util::ErrStatus(
        "Only GZIP and proto trace types are supported by DecompressTrace");
  }

  if (type == TraceType::kGzipTraceType) {
    std::unique_ptr<ChunkedTraceReader> reader(
        new SerializingProtoTraceReader(output));
    GzipTraceParser parser(std::move(reader));

    RETURN_IF_ERROR(parser.ParseUnowned(data, size));
    if (parser.needs_more_input())
      return util::ErrStatus("Cannot decompress partial trace file");

    parser.NotifyEndOfFile();
    return util::OkStatus();
  }

  PERFETTO_CHECK(type == TraceType::kProtoTraceType);

  protos::pbzero::Trace::Decoder decoder(data, size);
  util::GzipDecompressor decompressor;
  if (size > 0 && !decoder.packet()) {
    return util::ErrStatus("Trace does not contain valid packets");
  }
  for (auto it = decoder.packet(); it; ++it) {
    protos::pbzero::TracePacket::Decoder packet(*it);
    if (!packet.has_compressed_packets()) {
      it->SerializeAndAppendTo(output);
      continue;
    }

    // Make sure that to reset the stream between the gzip streams.
    auto bytes = packet.compressed_packets();
    decompressor.Reset();
    using ResultCode = util::GzipDecompressor::ResultCode;
    ResultCode ret = decompressor.FeedAndExtract(
        bytes.data, bytes.size, [&output](const uint8_t* buf, size_t buf_len) {
          output->insert(output->end(), buf, buf + buf_len);
        });
    if (ret == ResultCode::kError || ret == ResultCode::kNeedsMoreInput) {
      return util::ErrStatus("Failed while decompressing stream");
    }
  }
  return util::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
