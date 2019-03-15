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

#include "src/profiling/memory/unwinding.h"

#include <sys/types.h>
#include <unistd.h>

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
#include "perfetto/base/string_utils.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/base/thread_task_runner.h"
#include "src/profiling/memory/wire_protocol.h"

namespace perfetto {
namespace profiling {
namespace {

size_t kMaxFrames = 1000;

#pragma GCC diagnostic push
// We do not care about deterministic destructor order.
#pragma GCC diagnostic ignored "-Wglobal-constructors"
#pragma GCC diagnostic ignored "-Wexit-time-destructors"
static std::vector<std::string> kSkipMaps{"heapprofd_client.so"};
#pragma GCC diagnostic pop

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

// Behaves as a pread64, emulating it if not already exposed by the standard
// library. Safe to use on 32bit platforms for addresses with the top bit set.
// Clobbers the |fd| seek position if emulating.
ssize_t ReadAtOffsetClobberSeekPos(int fd,
                                   void* buf,
                                   size_t count,
                                   uint64_t addr) {
#ifdef __BIONIC__
  return pread64(fd, buf, count, static_cast<off64_t>(addr));
#else
  if (lseek64(fd, static_cast<off64_t>(addr), SEEK_SET) == -1)
    return -1;
  return read(fd, buf, count);
#endif
}

}  // namespace

StackOverlayMemory::StackOverlayMemory(std::shared_ptr<unwindstack::Memory> mem,
                                       uint64_t sp,
                                       uint8_t* stack,
                                       size_t size)
    : mem_(std::move(mem)), sp_(sp), stack_end_(sp + size), stack_(stack) {}

size_t StackOverlayMemory::Read(uint64_t addr, void* dst, size_t size) {
  if (addr >= sp_ && addr + size <= stack_end_ && addr + size > sp_) {
    size_t offset = static_cast<size_t>(addr - sp_);
    memcpy(dst, stack_ + offset, size);
    return size;
  }

  return mem_->Read(addr, dst, size);
}

FDMemory::FDMemory(base::ScopedFile mem_fd) : mem_fd_(std::move(mem_fd)) {}

size_t FDMemory::Read(uint64_t addr, void* dst, size_t size) {
  ssize_t rd = ReadAtOffsetClobberSeekPos(*mem_fd_, dst, size, addr);
  if (rd == -1) {
    PERFETTO_DPLOG("read of %zu at offset %" PRIu64, size, addr);
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
                       uint64_t pgoff, ino_t, const char* name) {
        // Mark a device map in /dev/ and not in /dev/ashmem/ specially.
        if (strncmp(name, "/dev/", 5) == 0 &&
            strncmp(name + 5, "ashmem/", 7) != 0) {
          flags |= unwindstack::MAPS_FLAGS_DEVICE_MAP;
        }
        maps_.emplace_back(
            new unwindstack::MapInfo(nullptr, start, end, pgoff, flags, name));
      });
}

void FileDescriptorMaps::Reset() {
  maps_.clear();
}

bool DoUnwind(WireMessage* msg, UnwindingMetadata* metadata, AllocRecord* out) {
  AllocMetadata* alloc_metadata = msg->alloc_header;
  std::unique_ptr<unwindstack::Regs> regs(
      CreateFromRawData(alloc_metadata->arch, alloc_metadata->register_data));
  if (regs == nullptr) {
    unwindstack::FrameData frame_data{};
    frame_data.function_name = "ERROR READING REGISTERS";
    frame_data.map_name = "ERROR";

    out->frames.emplace_back(frame_data, "");
    PERFETTO_DLOG("regs");
    return false;
  }
  uint8_t* stack = reinterpret_cast<uint8_t*>(msg->payload);
  std::shared_ptr<unwindstack::Memory> mems =
      std::make_shared<StackOverlayMemory>(metadata->fd_mem,
                                           alloc_metadata->stack_pointer, stack,
                                           msg->payload_size);

  unwindstack::Unwinder unwinder(kMaxFrames, &metadata->maps, regs.get(), mems);
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  unwinder.SetJitDebug(metadata->jit_debug.get(), regs->Arch());
  unwinder.SetDexFiles(metadata->dex_files.get(), regs->Arch());
#endif
  // Surpress incorrect "variable may be uninitialized" error for if condition
  // after this loop. error_code = LastErrorCode gets run at least once.
  uint8_t error_code = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (attempt > 0) {
      PERFETTO_DLOG("Reparsing maps");
      metadata->ReparseMaps();
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
      unwinder.SetJitDebug(metadata->jit_debug.get(), regs->Arch());
      unwinder.SetDexFiles(metadata->dex_files.get(), regs->Arch());
#endif
    }
    unwinder.Unwind(&kSkipMaps, nullptr);
    error_code = unwinder.LastErrorCode();
    if (error_code != unwindstack::ERROR_INVALID_MAP)
      break;
  }
  std::vector<unwindstack::FrameData> frames = unwinder.ConsumeFrames();
  for (unwindstack::FrameData& fd : frames) {
    std::string build_id;
    if (fd.map_name != "") {
      unwindstack::MapInfo* map_info = metadata->maps.Find(fd.pc);
      if (map_info)
        build_id = map_info->GetBuildID();
    }

    out->frames.emplace_back(std::move(fd), std::move(build_id));
  }

  if (error_code != 0) {
    PERFETTO_DLOG("Unwinding error %d", error_code);
    unwindstack::FrameData frame_data{};
    frame_data.function_name = "ERROR " + std::to_string(error_code);
    frame_data.map_name = "ERROR";

    out->frames.emplace_back(frame_data, "");
    PERFETTO_DLOG("unwinding failed %" PRIu8, error_code);
  }

  return true;
}

void UnwindingWorker::OnDisconnect(base::UnixSocket* self) {
  // TODO(fmayer): Maybe try to drain shmem one last time.
  auto it = client_data_.find(self->peer_pid());
  if (it == client_data_.end()) {
    PERFETTO_DFATAL("Disconnected unexpecter socket.");
    return;
  }
  ClientData& socket_data = it->second;
  DataSourceInstanceID ds_id = socket_data.data_source_instance_id;
  client_data_.erase(it);
  delegate_->PostSocketDisconnected(ds_id, self->peer_pid());
}

void UnwindingWorker::OnDataAvailable(base::UnixSocket* self) {
  // Drain buffer to clear the notification.
  char recv_buf[1024];
  self->Receive(recv_buf, sizeof(recv_buf));

  auto it = client_data_.find(self->peer_pid());
  if (it == client_data_.end()) {
    PERFETTO_DFATAL("Unexpected data.");
    return;
  }

  ClientData& socket_data = it->second;
  SharedRingBuffer& shmem = socket_data.shmem;
  SharedRingBuffer::Buffer buf;

  for (;;) {
    // TODO(fmayer): Allow spinlock acquisition to fail and repost Task if it
    // did.
    buf = shmem.BeginRead();
    if (!buf)
      break;
    HandleBuffer(&buf, &socket_data.metadata,
                 socket_data.data_source_instance_id,
                 socket_data.sock->peer_pid(), delegate_);
    shmem.EndRead(std::move(buf));
  }
}

// static
void UnwindingWorker::HandleBuffer(SharedRingBuffer::Buffer* buf,
                                   UnwindingMetadata* unwinding_metadata,
                                   DataSourceInstanceID data_source_instance_id,
                                   pid_t peer_pid,
                                   Delegate* delegate) {
  WireMessage msg;
  // TODO(fmayer): standardise on char* or uint8_t*.
  // char* has stronger guarantees regarding aliasing.
  // see https://timsong-cpp.github.io/cppwp/n3337/basic.lval#10.8
  if (!ReceiveWireMessage(reinterpret_cast<char*>(buf->data), buf->size,
                          &msg)) {
    PERFETTO_DFATAL("Failed to receive wire message.");
    return;
  }

  if (msg.record_type == RecordType::Malloc) {
    AllocRecord rec;
    rec.alloc_metadata = *msg.alloc_header;
    rec.pid = peer_pid;
    rec.data_source_instance_id = data_source_instance_id;
    DoUnwind(&msg, unwinding_metadata, &rec);
    delegate->PostAllocRecord(std::move(rec));
  } else if (msg.record_type == RecordType::Free) {
    FreeRecord rec;
    rec.pid = peer_pid;
    rec.data_source_instance_id = data_source_instance_id;
    // We need to copy this, so we can return the memory to the shmem buffer.
    memcpy(&rec.metadata, msg.free_header, sizeof(*msg.free_header));
    delegate->PostFreeRecord(std::move(rec));
  } else {
    PERFETTO_DFATAL("Invalid record type.");
  }
}

void UnwindingWorker::PostHandoffSocket(HandoffData handoff_data) {
  // Even with C++14, this cannot be moved, as std::function has to be
  // copyable, which HandoffData is not.
  HandoffData* raw_data = new HandoffData(std::move(handoff_data));
  // We do not need to use a WeakPtr here because the task runner will not
  // outlive its UnwindingWorker.
  thread_task_runner_.get()->PostTask([this, raw_data] {
    HandoffData data = std::move(*raw_data);
    delete raw_data;
    HandleHandoffSocket(std::move(data));
  });
}

void UnwindingWorker::HandleHandoffSocket(HandoffData handoff_data) {
  auto sock = base::UnixSocket::AdoptConnected(
      handoff_data.sock.ReleaseFd(), this, this->thread_task_runner_.get(),
      base::SockType::kStream);
  pid_t peer_pid = sock->peer_pid();

  UnwindingMetadata metadata(peer_pid,
                             std::move(handoff_data.fds[kHandshakeMaps]),
                             std::move(handoff_data.fds[kHandshakeMem]));
  ClientData client_data{
      handoff_data.data_source_instance_id, std::move(sock),
      std::move(metadata), std::move(handoff_data.shmem),
  };
  client_data_.emplace(peer_pid, std::move(client_data));
}

void UnwindingWorker::PostDisconnectSocket(pid_t pid) {
  // We do not need to use a WeakPtr here because the task runner will not
  // outlive its UnwindingWorker.
  thread_task_runner_.get()->PostTask(
      [this, pid] { HandleDisconnectSocket(pid); });
}

void UnwindingWorker::HandleDisconnectSocket(pid_t pid) {
  client_data_.erase(pid);
}

UnwindingWorker::Delegate::~Delegate() = default;

}  // namespace profiling
}  // namespace perfetto
