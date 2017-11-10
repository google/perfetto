/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <stdio.h>
#include <unistd.h>

#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ftrace_reader/ftrace_controller.h"
#include "ftrace_reader/ftrace_cpu_reader.h"
#include "protozero/scattered_stream_writer.h"

namespace {

std::string ToHex(const void* data, size_t length) {
  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  ss << std::uppercase;
  for (size_t i = 0; i < length; i++) {
    char c = reinterpret_cast<const char*>(data)[i];
    ss << std::setw(2) << (static_cast<unsigned>(c) & 0xFF);
  }
  return ss.str();
}

}  // namespace

class ScatteredBuffer : public protozero::ScatteredStreamWriter::Delegate {
 public:
  explicit ScatteredBuffer(size_t chunk_size);
  ~ScatteredBuffer() override;

  // protozero::ScatteredStreamWriter::Delegate implementation.
  protozero::ContiguousMemoryRange GetNewBuffer() override;

  std::string GetChunkAsString(size_t chunk_index);

  void GetBytes(size_t start, size_t length, uint8_t* buf);
  std::string GetBytesAsString(size_t start, size_t length);

  const std::vector<std::unique_ptr<uint8_t[]>>& chunks() const {
    return chunks_;
  }

 private:
  const size_t chunk_size_;
  std::vector<std::unique_ptr<uint8_t[]>> chunks_;
};

ScatteredBuffer::ScatteredBuffer(size_t chunk_size) : chunk_size_(chunk_size) {}

ScatteredBuffer::~ScatteredBuffer() {}

protozero::ContiguousMemoryRange ScatteredBuffer::GetNewBuffer() {
  std::unique_ptr<uint8_t[]> chunk(new uint8_t[chunk_size_]);
  uint8_t* begin = chunk.get();
  memset(begin, 0xff, chunk_size_);
  chunks_.push_back(std::move(chunk));
  return {begin, begin + chunk_size_};
}

std::string ScatteredBuffer::GetChunkAsString(size_t chunk_index) {
  return ToHex(chunks_[chunk_index].get(), chunk_size_);
}

void ScatteredBuffer::GetBytes(size_t start, size_t length, uint8_t* buf) {
  PERFETTO_CHECK(start + length < chunks_.size() * chunk_size_);
  for (size_t pos = 0; pos < length; ++pos) {
    size_t chunk_index = (start + pos) / chunk_size_;
    size_t chunk_offset = (start + pos) % chunk_size_;
    buf[pos] = chunks_[chunk_index].get()[chunk_offset];
  }
}

std::string ScatteredBuffer::GetBytesAsString(size_t start, size_t length) {
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[length]);
  GetBytes(start, length, buffer.get());
  return ToHex(buffer.get(), length);
}

int main(int argc, const char** argv) {
  auto ftrace = perfetto::FtraceController::Create();

  ftrace->ClearTrace();
  ftrace->WriteTraceMarker("Hello, world!");

  for (int i = 1; i < argc; i++) {
    printf("Enabling: %s\n", argv[i]);
    ftrace->EnableEvent(argv[i]);
  }

  // Sleep for one second so we get some events
  sleep(1);

  ScatteredBuffer buffer(4096);
  protozero::ScatteredStreamWriter stream_writer(&buffer);
  pbzero::FtraceEventBundle message;
  message.Reset(&stream_writer);
  perfetto::FtraceCpuReader* reader = ftrace->GetCpuReader(0);
  reader->Read(perfetto::FtraceCpuReader::Config(), &message);

  for (int i = 1; i < argc; i++) {
    printf("Disable: %s\n", argv[i]);
    ftrace->DisableEvent(argv[i]);
  }

  return 0;
}
