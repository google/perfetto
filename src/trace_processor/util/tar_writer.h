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

#ifndef SRC_TRACE_PROCESSOR_UTIL_TAR_WRITER_H_
#define SRC_TRACE_PROCESSOR_UTIL_TAR_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/scoped_file.h"

namespace perfetto::trace_processor::util {

// Interface for writing raw bytes to a destination.
class TarWriterSink {
 public:
  virtual ~TarWriterSink();
  virtual base::Status Write(const void* data, size_t len) = 0;
  virtual base::Status WriteFromFd(int fd, size_t len) = 0;
};

// Appends to an in-memory byte buffer.
class BufferTarWriterSink : public TarWriterSink {
 public:
  explicit BufferTarWriterSink(std::vector<uint8_t>* buffer);
  base::Status Write(const void* data, size_t len) override;
  base::Status WriteFromFd(int fd, size_t len) override;

 private:
  std::vector<uint8_t>* buffer_;
};

// Invokes a callback for each write, forwarding raw bytes.
class CallbackTarWriterSink : public TarWriterSink {
 public:
  using WriteCallback = std::function<void(const void* data, size_t len)>;
  explicit CallbackTarWriterSink(WriteCallback callback);
  base::Status Write(const void* data, size_t len) override;
  base::Status WriteFromFd(int fd, size_t len) override;

 private:
  WriteCallback callback_;
};

// Simple TAR writer that creates uncompressed TAR archives.
//
// Implements the POSIX ustar format for maximum compatibility:
// - Supported by all modern TAR implementations
// - Simple structure with fixed 512-byte blocks
// - No compression (keeps implementation simple and fast)
// - Supports files up to ~8GB with standard ustar format
//
// The ustar format was chosen over other TAR variants because:
// - GNU TAR extensions would limit compatibility
// - pax format adds complexity for minimal benefit in our use case
// - Original TAR format has more limitations (no long filenames)
class TarWriter {
 public:
  // Creates a TarWriter that writes to the given file path.
  explicit TarWriter(const std::string& output_path);

  // Creates a TarWriter that writes to the given file descriptor.
  explicit TarWriter(base::ScopedFile output_file);

  // Creates a TarWriter that uses a caller-provided sink.
  // The sink is not owned; it must outlive the TarWriter.
  explicit TarWriter(TarWriterSink* sink);

  ~TarWriter();

  // Adds a file to the TAR archive.
  // filename: The name of the file in the archive (max 100 chars)
  // content: The file content as a string
  base::Status AddFile(const std::string& filename, const std::string& content);

  // Adds a file to the TAR archive.
  // filename: The name of the file in the archive (max 100 chars)
  // data/size: The file content as raw bytes
  base::Status AddFile(const std::string& filename,
                       const uint8_t* data,
                       size_t size);

  // Adds a file to the TAR archive from a file path.
  // filename: The name of the file in the archive (max 100 chars)
  // file_path: Path to the file to add
  base::Status AddFileFromPath(const std::string& filename,
                               const std::string& file_path);

 private:
  // TAR header structure (512 bytes)
  struct TarHeader {
    char name[100];      // File name
    char mode[8];        // File mode (octal)
    char uid[8];         // User ID (octal)
    char gid[8];         // Group ID (octal)
    char size[12];       // File size in bytes (octal)
    char mtime[12];      // Modification time (octal)
    char checksum[8];    // Header checksum
    char typeflag;       // File type
    char linkname[100];  // Name of linked file
    char magic[6];       // USTAR indicator
    char version[2];     // USTAR version
    char uname[32];      // User name
    char gname[32];      // Group name
    char devmajor[8];    // Device major number
    char devminor[8];    // Device minor number
    char prefix[155];    // Filename prefix
    char padding[12];    // Padding to 512 bytes
  };
  static_assert(sizeof(TarHeader) == 512, "TarHeader must be 512 bytes");

  base::Status ValidateFilename(const std::string& filename);
  base::Status CreateAndWriteHeader(const std::string& filename,
                                    size_t file_size);
  base::Status WritePadding(size_t size);
  void Finalize();

  // Owned sink created by convenience constructors, null if caller-provided.
  std::unique_ptr<TarWriterSink> owned_sink_;
  // Points to either owned_sink_ or a caller-provided sink.
  TarWriterSink* sink_;
  bool finalized_ = false;
};

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_TAR_WRITER_H_
