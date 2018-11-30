/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <unwindstack/MachineArm.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/MachineMips.h>
#include <unwindstack/MachineMips64.h>
#include <unwindstack/MachineX86.h>
#include <unwindstack/MachineX86_64.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Memory.h>
#include <unwindstack/Regs.h>
#include <unwindstack/RegsArm.h>
#include <unwindstack/RegsArm64.h>
#include <unwindstack/RegsMips.h>
#include <unwindstack/RegsMips64.h>
#include <unwindstack/RegsX86.h>
#include <unwindstack/RegsX86_64.h>
#include <unwindstack/Unwinder.h>
#include <unwindstack/UserArm.h>
#include <unwindstack/UserArm64.h>
#include <unwindstack/UserMips.h>
#include <unwindstack/UserMips64.h>
#include <unwindstack/UserX86.h>
#include <unwindstack/UserX86_64.h>

#include <procinfo/process_map.h>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "src/profiling/memory/unwinding.h"
#include "src/profiling/memory/wire_protocol.h"

namespace perfetto {
namespace profiling {
namespace {

size_t kMaxFrames = 1000;

std::unique_ptr<unwindstack::Regs> CreateFromRawData(unwindstack::ArchEnum arch,
                                                     void* raw_data) {
  std::unique_ptr<unwindstack::Regs> ret;
  // unwindstack::RegsX::Read returns a raw ptr which we are expected to free.
  switch (arch) {
    case unwindstack::ARCH_X86:
      ret.reset(unwindstack::RegsX86::Read(raw_data));
      break;
    case unwindstack::ARCH_X86_64:
      ret.reset(unwindstack::RegsX86_64::Read(raw_data));
      break;
    case unwindstack::ARCH_ARM:
      ret.reset(unwindstack::RegsArm::Read(raw_data));
      break;
    case unwindstack::ARCH_ARM64:
      ret.reset(unwindstack::RegsArm64::Read(raw_data));
      break;
    case unwindstack::ARCH_MIPS:
      ret.reset(unwindstack::RegsMips::Read(raw_data));
      break;
    case unwindstack::ARCH_MIPS64:
      ret.reset(unwindstack::RegsMips64::Read(raw_data));
      break;
    case unwindstack::ARCH_UNKNOWN:
      ret.reset(nullptr);
      break;
  }
  return ret;
}

}  // namespace

StackMemory::StackMemory(int mem_fd, uint64_t sp, uint8_t* stack, size_t size)
    : mem_fd_(mem_fd), sp_(sp), stack_end_(sp + size), stack_(stack) {}

size_t StackMemory::Read(uint64_t addr, void* dst, size_t size) {
  if (addr >= sp_ && addr + size <= stack_end_ && addr + size > sp_) {
    size_t offset = static_cast<size_t>(addr - sp_);
    memcpy(dst, stack_ + offset, size);
    return size;
  }

  if (lseek(mem_fd_, static_cast<off_t>(addr), SEEK_SET) == -1)
    return 0;

  ssize_t rd = read(mem_fd_, dst, size);
  if (rd == -1) {
    PERFETTO_DPLOG("read");
    return 0;
  }
  return static_cast<size_t>(rd);
}

FileDescriptorMaps::FileDescriptorMaps(base::ScopedFile fd)
    : fd_(std::move(fd)) {}

bool FileDescriptorMaps::Parse() {
  // If the process has already exited, lseek or ReadFileDescriptor will
  // return false.
  if (lseek(*fd_, 0, SEEK_SET) == -1)
    return false;

  std::string content;
  if (!base::ReadFileDescriptor(*fd_, &content))
    return false;
  return android::procinfo::ReadMapFileContent(
      &content[0], [&](uint64_t start, uint64_t end, uint16_t flags,
                       uint64_t pgoff, const char* name) {
        // Mark a device map in /dev/ and not in /dev/ashmem/ specially.
        if (strncmp(name, "/dev/", 5) == 0 &&
            strncmp(name + 5, "ashmem/", 7) != 0) {
          flags |= unwindstack::MAPS_FLAGS_DEVICE_MAP;
        }
        maps_.push_back(
            new unwindstack::MapInfo(nullptr, start, end, pgoff, flags, name));
      });
}

void FileDescriptorMaps::Reset() {
  for (unwindstack::MapInfo* info : maps_)
    delete info;
  maps_.clear();
}

bool DoUnwind(WireMessage* msg, UnwindingMetadata* metadata, AllocRecord* out) {
  AllocMetadata* alloc_metadata = msg->alloc_header;
  std::unique_ptr<unwindstack::Regs> regs(
      CreateFromRawData(alloc_metadata->arch, alloc_metadata->register_data));
  if (regs == nullptr) {
    PERFETTO_DLOG("regs");
    return false;
  }
  out->alloc_metadata = *alloc_metadata;
  uint8_t* stack = reinterpret_cast<uint8_t*>(msg->payload);
  std::shared_ptr<unwindstack::Memory> mems = std::make_shared<StackMemory>(
      *metadata->mem_fd, alloc_metadata->stack_pointer, stack,
      msg->payload_size);
  unwindstack::Unwinder unwinder(kMaxFrames, &metadata->maps, regs.get(), mems);
  // Surpress incorrect "variable may be uninitialized" error for if condition
  // after this loop. error_code = LastErrorCode gets run at least once.
  uint8_t error_code = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (attempt > 0) {
      metadata->maps.Reset();
      metadata->maps.Parse();
    }
    unwinder.Unwind();
    error_code = unwinder.LastErrorCode();
    if (error_code != unwindstack::ERROR_INVALID_MAP)
      break;
  }
  out->frames = unwinder.frames();
  if (error_code != 0) {
    unwindstack::FrameData frame_data{};
    frame_data.function_name = "ERROR " + std::to_string(error_code);
    frame_data.map_name = "ERROR";

    out->frames.emplace_back(frame_data);
    PERFETTO_DLOG("unwinding failed %" PRIu8, error_code);
  }
  return true;
}

bool HandleUnwindingRecord(UnwindingRecord* rec, BookkeepingRecord* out) {
  WireMessage msg;
  if (!ReceiveWireMessage(reinterpret_cast<char*>(rec->data.get()), rec->size,
                          &msg))
    return false;
  if (msg.record_type == RecordType::Malloc) {
    std::shared_ptr<UnwindingMetadata> metadata = rec->metadata.lock();
    if (!metadata) {
      // Process has already gone away.
      return false;
    }

    out->pid = rec->pid;
    out->record_type = BookkeepingRecord::Type::Malloc;
    return DoUnwind(&msg, metadata.get(), &out->alloc_record);
  } else if (msg.record_type == RecordType::Free) {
    out->record_type = BookkeepingRecord::Type::Free;
    out->pid = rec->pid;
    // We need to keep this alive, because msg.free_header is a pointer into
    // this.
    out->free_record.free_data = std::move(rec->data);
    out->free_record.metadata = msg.free_header;
    return true;
  } else {
    PERFETTO_DFATAL("Invalid record type.");
    return false;
  }
}

void UnwindingMainLoop(BoundedQueue<UnwindingRecord>* input_queue,
                       BoundedQueue<BookkeepingRecord>* output_queue) {
  for (;;) {
    UnwindingRecord rec;
    if (!input_queue->Get(&rec))
      return;
    BookkeepingRecord out;
    if (HandleUnwindingRecord(&rec, &out))
      output_queue->Add(std::move(out));
  }
}

}  // namespace profiling
}  // namespace perfetto
