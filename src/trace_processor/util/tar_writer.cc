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

#include "src/trace_processor/util/tar_writer.h"

#include <fcntl.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_processor::util {

namespace {
// Helper function to safely copy from constant string arrays into fixed-size
// char arrays
template <size_t DestN, size_t SrcN>
void SafeCopyToCharArray(char (&dest)[DestN], const char (&src)[SrcN]) {
  static_assert(SrcN - 1 <= DestN,
                "Source string too long for destination array");
  constexpr size_t copy_len =
      SrcN - 1;  // -1 to exclude null terminator from src
  memcpy(dest, src, copy_len);
  // Zero-fill the rest
  if constexpr (copy_len < DestN) {
    memset(dest + copy_len, 0, DestN - copy_len);
  }
}
// Writes to a file descriptor. Internal-only; not exposed in the header.
class FdTarWriterSink : public TarWriterSink {
 public:
  explicit FdTarWriterSink(base::ScopedFile fd) : fd_(std::move(fd)) {
    PERFETTO_CHECK(fd_);
  }
  base::Status Write(const void* data, size_t len) override {
    ssize_t written =
        base::WriteAll(fd_.get(), static_cast<const char*>(data), len);
    if (written != static_cast<ssize_t>(len))
      return base::ErrStatus("Failed to write to TAR output");
    return base::OkStatus();
  }
  base::Status WriteFromFd(int fd, size_t /*len*/) override {
    // Efficient fd-to-fd copy without intermediate buffer.
    return base::CopyFileContents(fd, *fd_);
  }

 private:
  base::ScopedFile fd_;
};

}  // namespace

// --- Sink implementations ---

TarWriterSink::~TarWriterSink() = default;

BufferTarWriterSink::BufferTarWriterSink(std::vector<uint8_t>* buffer)
    : buffer_(buffer) {
  PERFETTO_CHECK(buffer_);
}

base::Status BufferTarWriterSink::Write(const void* data, size_t len) {
  auto* bytes = static_cast<const uint8_t*>(data);
  buffer_->insert(buffer_->end(), bytes, bytes + len);
  return base::OkStatus();
}

base::Status BufferTarWriterSink::WriteFromFd(int fd, size_t len) {
  size_t old_size = buffer_->size();
  buffer_->resize(old_size + len);
  ssize_t rd =
      base::Read(fd, reinterpret_cast<char*>(buffer_->data() + old_size), len);
  if (rd != static_cast<ssize_t>(len)) {
    buffer_->resize(old_size);
    return base::ErrStatus("Failed to read from fd");
  }
  return base::OkStatus();
}

// --- TarWriter ---

TarWriter::TarWriter(const std::string& output_path)
    : TarWriter(
          base::OpenFile(output_path, O_CREAT | O_WRONLY | O_TRUNC, 0644)) {}

TarWriter::TarWriter(base::ScopedFile output_file)
    : owned_sink_(new FdTarWriterSink(std::move(output_file))),
      sink_(owned_sink_.get()) {}

TarWriter::TarWriter(TarWriterSink* sink) : sink_(sink) {
  PERFETTO_CHECK(sink_);
}

TarWriter::~TarWriter() {
  Finalize();
}

void TarWriter::Finalize() {
  if (finalized_)
    return;
  finalized_ = true;
  // Write two 512-byte blocks of zeros to mark end of archive.
  char zero_block[512] = {0};
  auto s1 = sink_->Write(zero_block, 512);
  PERFETTO_CHECK(s1.ok());
  auto s2 = sink_->Write(zero_block, 512);
  PERFETTO_CHECK(s2.ok());
}

base::Status TarWriter::AddFile(const std::string& filename,
                                const std::string& content) {
  return AddFile(filename, reinterpret_cast<const uint8_t*>(content.data()),
                 content.size());
}

base::Status TarWriter::AddFile(const std::string& filename,
                                const uint8_t* data,
                                size_t size) {
  RETURN_IF_ERROR(ValidateFilename(filename));
  RETURN_IF_ERROR(CreateAndWriteHeader(filename, size));
  RETURN_IF_ERROR(sink_->Write(data, size));
  RETURN_IF_ERROR(WritePadding(size));
  return base::OkStatus();
}

base::Status TarWriter::AddFileFromPath(const std::string& filename,
                                        const std::string& file_path) {
  RETURN_IF_ERROR(ValidateFilename(filename));

  // Get file size
  auto file_size_opt = base::GetFileSize(file_path);
  if (!file_size_opt)
    return base::ErrStatus("Failed to get file size: %s", file_path.c_str());
  size_t file_size = static_cast<size_t>(*file_size_opt);

  base::ScopedFile file = base::OpenFile(file_path, O_RDONLY);
  if (!file)
    return base::ErrStatus("Failed to open file: %s", file_path.c_str());

  RETURN_IF_ERROR(CreateAndWriteHeader(filename, file_size));
  RETURN_IF_ERROR(sink_->WriteFromFd(*file, file_size));

  RETURN_IF_ERROR(WritePadding(file_size));
  return base::OkStatus();
}

base::Status TarWriter::CreateAndWriteHeader(const std::string& filename,
                                             size_t file_size) {
  TarHeader header;

  // Initialize header
  memset(&header, 0, sizeof(TarHeader));
  SafeCopyToCharArray(header.mode, "0644   ");   // Regular file, rw-r--r--
  SafeCopyToCharArray(header.uid, "0000000");    // Root user
  SafeCopyToCharArray(header.gid, "0000000");    // Root group
  header.typeflag = '0';                         // Regular file
  SafeCopyToCharArray(header.magic, "ustar\0");  // POSIX ustar format
  SafeCopyToCharArray(header.version, "00");     // Version
  SafeCopyToCharArray(header.uname, "root");     // User name
  SafeCopyToCharArray(header.gname, "root");     // Group name
  SafeCopyToCharArray(header.devmajor, "0000000");
  SafeCopyToCharArray(header.devminor, "0000000");
  memset(header.checksum, ' ', sizeof(header.checksum));

  // Set filename
  base::StringCopy(header.name, filename.c_str(), sizeof(header.name));

  // Set file size (in octal)
  snprintf(header.size, sizeof(header.size), "%011lo",
           static_cast<unsigned long>(file_size));

  // Set modification time to current time (in octal)
  snprintf(header.mtime, sizeof(header.mtime), "%011lo",
           static_cast<unsigned long>(time(nullptr)));

  // Compute checksum
  unsigned int sum = 0;
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&header);
  for (size_t i = 0; i < sizeof(TarHeader); i++)
    sum += bytes[i];
  snprintf(header.checksum, sizeof(header.checksum), "%06o", sum);
  header.checksum[6] = '\0';
  header.checksum[7] = ' ';

  // Write header
  return sink_->Write(&header, sizeof(header));
}

base::Status TarWriter::WritePadding(size_t size) {
  // TAR files must be padded to 512-byte boundaries
  size_t padding_needed = (512 - (size % 512)) % 512;
  if (padding_needed > 0) {
    char zeros[512] = {0};
    RETURN_IF_ERROR(sink_->Write(zeros, padding_needed));
  }
  return base::OkStatus();
}

base::Status TarWriter::ValidateFilename(const std::string& filename) {
  // TAR header name field is 100 bytes, but we need null termination
  if (filename.empty())
    return base::ErrStatus("Filename cannot be empty");
  if (filename.length() > 99)
    return base::ErrStatus(
        "Filename too long for TAR format (max 99 chars): %s",
        filename.c_str());
  // Check for invalid characters that might cause issues
  if (filename.find('\0') != std::string::npos)
    return base::ErrStatus("Filename contains null character");
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::util
