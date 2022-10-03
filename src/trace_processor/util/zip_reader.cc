/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/util/zip_reader.h"

#include <time.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/util/gzip_utils.h"
#include "src/trace_processor/util/streaming_line_reader.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
#include <zlib.h>  // For crc32().
#endif

namespace perfetto {
namespace trace_processor {
namespace util {

namespace {

// Entry signatures.
const uint32_t kFileHeaderSig = 0x04034b50;
const uint32_t kCentralDirectorySig = 0x02014b50;

// Compression flags.
const uint16_t kNoCompression = 0;
const uint16_t kDeflate = 8;

template <typename T>
T ReadAndAdvance(const uint8_t** ptr) {
  T res{};
  memcpy(base::AssumeLittleEndian(&res), *ptr, sizeof(T));
  *ptr += sizeof(T);
  return res;
}

}  // namespace

ZipReader::ZipReader() = default;
ZipReader::~ZipReader() = default;

base::Status ZipReader::Parse(const void* data, size_t len) {
  const uint8_t* input = static_cast<const uint8_t*>(data);
  const uint8_t* const input_begin = input;
  const uint8_t* const input_end = input + len;
  auto input_avail = [&] { return static_cast<size_t>(input_end - input); };

  // .zip file sequence:
  // [ File 1 header (30 bytes) ]
  // [ File 1 name ]
  // [ File 1 extra fields (optional) ]
  // [ File 1 compressed payload ]
  //
  // [ File 2 header (30 bytes) ]
  // [ File 2 name ]
  // [ File 2 extra fields (optional) ]
  // [ File 2 compressed payload ]
  //
  // [ Central directory (ignored) ]
  while (input < input_end) {
    // Initial state, we are building up the file header.
    if (cur_.raw_hdr_size < kZipFileHdrSize) {
      size_t copy_size =
          std::min(input_avail(), kZipFileHdrSize - cur_.raw_hdr_size);
      memcpy(&cur_.raw_hdr[cur_.raw_hdr_size], input, copy_size);
      cur_.raw_hdr_size += copy_size;
      input += copy_size;

      // If we got all the kZipFileHdrSize bytes, parse the zip file header now.
      if (cur_.raw_hdr_size == kZipFileHdrSize) {
        const uint8_t* hdr_it = &cur_.raw_hdr[0];
        cur_.hdr.signature = ReadAndAdvance<uint32_t>(&hdr_it);
        if (cur_.hdr.signature == kCentralDirectorySig) {
          // We reached the central directory at the end of file.
          // We don't make any use here of the central directory, so we just
          // ignore everything else after this point.
          // Here we abuse the ZipFile class a bit. The Central Directory header
          // has a different layout. The first 4 bytes (signature) match, the
          // rest don't but the sizeof(central dir) is >> sizeof(file header) so
          // we are fine.
          // We do this rather than retuning because we could have further
          // Parse() calls (imagine parsing bytes one by one), and we need a way
          // to keep track of the "keep eating input without doing anything".
          cur_.ignore_bytes_after_fname = std::numeric_limits<size_t>::max();
          input = input_end;
          break;
        }
        if (cur_.hdr.signature != kFileHeaderSig) {
          return base::ErrStatus(
              "Invalid signature found at offset 0x%zx. Actual=%x, expected=%x",
              static_cast<size_t>(input - input_begin) - kZipFileHdrSize,
              cur_.hdr.signature, kFileHeaderSig);
        }

        cur_.hdr.version = ReadAndAdvance<uint16_t>(&hdr_it);
        cur_.hdr.flags = ReadAndAdvance<uint16_t>(&hdr_it);
        cur_.hdr.compression = ReadAndAdvance<uint16_t>(&hdr_it);
        cur_.hdr.mtime = ReadAndAdvance<uint16_t>(&hdr_it);
        cur_.hdr.mdate = ReadAndAdvance<uint16_t>(&hdr_it);
        cur_.hdr.checksum = ReadAndAdvance<uint32_t>(&hdr_it);
        cur_.hdr.compressed_size = ReadAndAdvance<uint32_t>(&hdr_it);
        cur_.hdr.uncompressed_size = ReadAndAdvance<uint32_t>(&hdr_it);
        cur_.hdr.fname_len = ReadAndAdvance<uint16_t>(&hdr_it);
        cur_.hdr.extra_field_len = ReadAndAdvance<uint16_t>(&hdr_it);
        PERFETTO_DCHECK(static_cast<size_t>(hdr_it - cur_.raw_hdr) ==
                        kZipFileHdrSize);

        // We support only up to version 2.0 (20). Higher versions define
        // more advanced features that we don't support (zip64 extensions,
        // encryption).
        // Flag bits 1,2 define the compression strength for deflating (which
        // zlib supports transparently). Other bits define other compression
        // methods that we don't support.
        if ((cur_.hdr.version > 20) || (cur_.hdr.flags & ~3) != 0) {
          return base::ErrStatus(
              "Unsupported zip features at offset 0x%zx. version=%x, flags=%x",
              static_cast<size_t>(input - input_begin) - kZipFileHdrSize,
              cur_.hdr.version, cur_.hdr.flags);
        }
        cur_.compressed_data.reset(new uint8_t[cur_.hdr.compressed_size]);
        cur_.ignore_bytes_after_fname = cur_.hdr.extra_field_len;
      }
      continue;
    }

    // Build up the file name.
    if (cur_.hdr.fname.size() < cur_.hdr.fname_len) {
      size_t name_left = cur_.hdr.fname_len - cur_.hdr.fname.size();
      size_t copy_size = std::min(name_left, input_avail());
      cur_.hdr.fname.append(reinterpret_cast<const char*>(input), copy_size);
      input += copy_size;
      continue;
    }

    // Skip any bytes if extra fields were present.
    if (cur_.ignore_bytes_after_fname > 0) {
      size_t skip_size = std::min(input_avail(), cur_.ignore_bytes_after_fname);
      cur_.ignore_bytes_after_fname -= skip_size;
      input += skip_size;
      continue;
    }

    // Build up the compressed payload
    if (cur_.compressed_data_written < cur_.hdr.compressed_size) {
      size_t needed = cur_.hdr.compressed_size - cur_.compressed_data_written;
      size_t copy_size = std::min(needed, input_avail());
      memcpy(&cur_.compressed_data[cur_.compressed_data_written], input,
             copy_size);
      cur_.compressed_data_written += copy_size;
      input += copy_size;
      continue;
    }

    // We have accumulated the whole header, file name and compressed payload.
    PERFETTO_DCHECK(cur_.raw_hdr_size == kZipFileHdrSize);
    PERFETTO_DCHECK(cur_.hdr.fname.size() == cur_.hdr.fname_len);
    PERFETTO_DCHECK(cur_.compressed_data_written == cur_.hdr.compressed_size);
    PERFETTO_DCHECK(cur_.ignore_bytes_after_fname == 0);

    files_.emplace_back();
    files_.back().hdr_ = std::move(cur_.hdr);
    files_.back().compressed_data_ = std::move(cur_.compressed_data);
    cur_ = FileParseState();  // Reset the parsing state for the next file.

  }  // while (input < input_end)

  // At this point we must have consumed all input.
  PERFETTO_DCHECK(input_avail() == 0);
  return base::OkStatus();
}

ZipFile* ZipReader::Find(const std::string& path) {
  for (ZipFile& zf : files_) {
    if (zf.name() == path)
      return &zf;
  }
  return nullptr;
}

ZipFile::ZipFile() = default;
ZipFile::~ZipFile() = default;
ZipFile::ZipFile(ZipFile&& other) noexcept = default;
ZipFile& ZipFile::operator=(ZipFile&& other) noexcept = default;

base::Status ZipFile::Decompress(std::vector<uint8_t>* out_data) const {
  out_data->clear();

  auto res = DoDecompressionChecks();
  if (!res.ok())
    return res;

  if (hdr_.compression == kNoCompression) {
    const uint8_t* data = compressed_data_.get();
    out_data->insert(out_data->end(), data, data + hdr_.compressed_size);
    return base::OkStatus();
  }

  if (hdr_.uncompressed_size == 0)
    return base::OkStatus();

  PERFETTO_DCHECK(hdr_.compression == kDeflate);
  GzipDecompressor dec(GzipDecompressor::InputMode::kRawDeflate);
  dec.Feed(compressed_data_.get(), hdr_.compressed_size);

  out_data->resize(hdr_.uncompressed_size);
  auto dec_res = dec.ExtractOutput(out_data->data(), out_data->size());
  if (dec_res.ret != GzipDecompressor::ResultCode::kEof) {
    return base::ErrStatus("Zip decompression error (%d) on %s (c=%u, u=%u)",
                           static_cast<int>(dec_res.ret), hdr_.fname.c_str(),
                           hdr_.compressed_size, hdr_.uncompressed_size);
  }
  out_data->resize(dec_res.bytes_written);

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
  const auto* crc_data = reinterpret_cast<const ::Bytef*>(out_data->data());
  auto crc_len = static_cast<::uInt>(out_data->size());
  auto actual_crc32 = static_cast<uint32_t>(::crc32(0u, crc_data, crc_len));
  if (actual_crc32 != hdr_.checksum) {
    return base::ErrStatus("Zip CRC32 failure on %s (actual: %x, expected: %x)",
                           hdr_.fname.c_str(), actual_crc32, hdr_.checksum);
  }
#endif

  return base::OkStatus();
}

base::Status ZipFile::DecompressLines(LinesCallback callback) const {
  using ResultCode = GzipDecompressor::ResultCode;

  auto res = DoDecompressionChecks();
  if (!res.ok())
    return res;

  StreamingLineReader line_reader(callback);

  if (hdr_.compression == kNoCompression) {
    line_reader.Tokenize(
        base::StringView(reinterpret_cast<const char*>(compressed_data_.get()),
                         hdr_.compressed_size));
    return base::OkStatus();
  }

  PERFETTO_DCHECK(hdr_.compression == kDeflate);
  GzipDecompressor dec(GzipDecompressor::InputMode::kRawDeflate);
  dec.Feed(compressed_data_.get(), hdr_.compressed_size);

  static constexpr size_t kChunkSize = 32768;
  GzipDecompressor::Result dec_res;
  do {
    auto* wptr = reinterpret_cast<uint8_t*>(line_reader.BeginWrite(kChunkSize));
    dec_res = dec.ExtractOutput(wptr, kChunkSize);
    if (dec_res.ret == ResultCode::kError ||
        dec_res.ret == ResultCode::kNeedsMoreInput)
      return base::ErrStatus("zlib decompression error on %s (%d)",
                             name().c_str(), static_cast<int>(dec_res.ret));
    PERFETTO_DCHECK(dec_res.bytes_written <= kChunkSize);
    line_reader.EndWrite(dec_res.bytes_written);
  } while (dec_res.ret == ResultCode::kOk);
  return base::OkStatus();
}

// Common logic for both Decompress() and DecompressLines().
base::Status ZipFile::DoDecompressionChecks() const {
  PERFETTO_DCHECK(compressed_data_);

  if (hdr_.compression == kNoCompression) {
    PERFETTO_CHECK(hdr_.compressed_size == hdr_.uncompressed_size);
    return base::OkStatus();
  }

  if (hdr_.compression != kDeflate) {
    return base::ErrStatus("Zip compression mode not supported (%u)",
                           hdr_.compression);
  }

  if (!IsGzipSupported()) {
    return base::ErrStatus(
        "Cannot open zip file. Gzip is not enabled in the current build. "
        "Rebuild with enable_perfetto_zlib=true");
  }

  return base::OkStatus();
}

// Returns a 64-bit version of time_t, that is, the num seconds since the Epoch.
int64_t ZipFile::GetDatetime() const {
  // Date: 7 bits year, 4 bits month, 5 bits day.
  // Time: 5 bits hour, 6 bits minute, 5 bits second.
  struct tm mdt {};
  // As per man 3 mktime, `tm_year` is relative to 1900 not Epoch. Go figure.
  mdt.tm_year = 1980 + (hdr_.mdate >> (16 - 7)) - 1900;

  // As per the man page, the month ranges 0 to 11 (Jan = 0).
  mdt.tm_mon = ((hdr_.mdate >> (16 - 7 - 4)) & 0x0f) - 1;

  // However, still according to the same man page, the day starts from 1.
  mdt.tm_mday = hdr_.mdate & 0x1f;

  mdt.tm_hour = hdr_.mtime >> (16 - 5);
  mdt.tm_min = (hdr_.mtime >> (16 - 5 - 6)) & 0x3f;

  // Seconds in the DOS format have only 5 bits, so they lose the last bit of
  // resolution, hence the * 2.
  mdt.tm_sec = (hdr_.mtime & 0x1f) * 2;
  return base::TimeGm(&mdt);
}

std::string ZipFile::GetDatetimeStr() const {
  char buf[32]{};
  time_t secs = static_cast<time_t>(GetDatetime());
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", gmtime(&secs));
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto
