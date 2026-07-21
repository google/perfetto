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
#include "perfetto/ext/base/status_or.h"

namespace perfetto::trace_processor::util {

// Destination for the bytes produced by a TarWriter.
class TarWriterSink {
 public:
  virtual ~TarWriterSink();
  virtual base::Status Write(const void* data, size_t len) = 0;
  // Copies len bytes from fd to the sink. Default implementation may read
  // into a buffer and call Write; fd-backed sinks can override with an
  // efficient fd-to-fd copy.
  virtual base::Status WriteFromFd(int fd, size_t len) = 0;
};

// Appends written bytes to an in-memory buffer.
class BufferTarWriterSink : public TarWriterSink {
 public:
  explicit BufferTarWriterSink(std::vector<uint8_t>* buffer);
  base::Status Write(const void* data, size_t len) override;
  base::Status WriteFromFd(int fd, size_t len) override;

 private:
  std::vector<uint8_t>* buffer_;
};

// Forwards written bytes to a caller-supplied callback, e.g. to stream a
// TAR archive over an RPC connection.
class CallbackTarWriterSink : public TarWriterSink {
 public:
  // A failing status aborts the write that triggered it; it is propagated
  // back to the TarWriter caller.
  using WriteCallback =
      std::function<base::Status(const void* data, size_t len)>;
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
  explicit TarWriter(const std::string& output_path);
  explicit TarWriter(base::ScopedFile output_file);
  // `sink` is not owned and must outlive the TarWriter.
  explicit TarWriter(TarWriterSink* sink);
  ~TarWriter();
  TarWriter(const TarWriter&) = delete;
  TarWriter& operator=(const TarWriter&) = delete;

  // Adds a file to the TAR archive.
  // filename: The name of the file in the archive (max 100 chars)
  // content: The file content
  // Returns OkStatus() on success, error Status on failure.
  base::Status AddFile(const std::string& filename, const std::string& content);

  // Adds a file to the TAR archive from raw bytes.
  base::Status AddFile(const std::string& filename,
                       const uint8_t* data,
                       size_t size);

  // Adds a file to the TAR archive from a file path.
  // filename: The name of the file in the archive (max 100 chars)
  // file_path: Path to the file to add
  // Returns OkStatus() on success, error Status on failure.
  base::Status AddFileFromPath(const std::string& filename,
                               const std::string& file_path);

  // Handle for streaming a single file's content into the archive without
  // buffering it all in memory first. Returned by BeginFile(). Move-only.
  class ScopedFileWriter {
   public:
    ScopedFileWriter(ScopedFileWriter&&) noexcept;
    ScopedFileWriter& operator=(ScopedFileWriter&&) noexcept;
    // Best-effort Finish() if not already called; logs (never crashes) on
    // failure, since a broken pipe on teardown must not abort the process.
    ~ScopedFileWriter();
    ScopedFileWriter(const ScopedFileWriter&) = delete;
    ScopedFileWriter& operator=(const ScopedFileWriter&) = delete;

    base::Status Write(const void* data, size_t len);

    // Writes the 512-byte-boundary padding. Must be called once after
    // exactly the `size` bytes passed to BeginFile() have been written.
    base::Status Finish();

   private:
    friend class TarWriter;
    ScopedFileWriter(TarWriterSink* sink, size_t size);

    TarWriterSink* sink_;
    size_t size_;
    bool finished_ = false;
  };

  // Writes the TAR header for `filename` immediately and returns a writer
  // for streaming exactly `size` bytes of content.
  base::StatusOr<ScopedFileWriter> BeginFile(const std::string& filename,
                                             size_t size);

  // Writes the two zero end-of-archive blocks. Idempotent: calls after the
  // first always return OkStatus() without writing anything further.
  base::Status Finalize();

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

  // Owned when constructed from a path/fd; null when a caller-provided sink
  // is used instead. `sink_` always points at the sink actually used.
  std::unique_ptr<TarWriterSink> owned_sink_;
  TarWriterSink* sink_;
  bool finalized_ = false;
};

}  // namespace perfetto::trace_processor::util

#endif  // SRC_TRACE_PROCESSOR_UTIL_TAR_WRITER_H_
