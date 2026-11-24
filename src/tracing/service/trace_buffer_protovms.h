/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACING_SERVICE_TRACE_BUFFER_PROTOVMS_H_
#define SRC_TRACING_SERVICE_TRACE_BUFFER_PROTOVMS_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/tracing/core/basic_types.h"

namespace perfetto {

class TracePacket;

namespace protovm {
class Vm;
}

// Owns the ProtoVM instances for one trace buffer (go/perfetto-proto-vm).
// When the buffer is about to overwrite a packet, it passes the packet to
// MaybeProcessOverwrittenPacket() to merge it into a ProtoVM's incremental
// state. The service emits this state when it reads the buffer.
//
// The buffer decides which packets to pass here. They must be length-delimited
// protobuf and arrive in write order within each packet sequence.
class TraceBufferProtoVms {
 public:
  // A ProtoVM instance and the producers whose packets it processes.
  struct Vm {
    Vm();
    ~Vm();
    Vm(Vm&&) noexcept;

    Vm CloneReadOnly() const;

    std::unique_ptr<protovm::Vm> instance;
    std::string data_source_name;
    uint64_t program_hash = 0;
    uint32_t memory_limit_kb = 0;
    base::FlatSet<ProducerID> producers;
  };

  TraceBufferProtoVms();
  ~TraceBufferProtoVms();

  TraceBufferProtoVms(const TraceBufferProtoVms&) = delete;
  TraceBufferProtoVms& operator=(const TraceBufferProtoVms&) = delete;

  // Adds |producer_id| to the ProtoVM that runs |program_bytes| for
  // |data_source_name|. Creates that ProtoVM if it does not exist.
  void MaybeSetUp(const std::string& data_source_name,
                  const std::string& program_bytes,
                  uint32_t memory_limit_kb,
                  ProducerID producer_id);

  // Tries each ProtoVM registered for |producer| until ApplyPatch() succeeds.
  // Does nothing if the producer has no ProtoVM.
  void MaybeProcessOverwrittenPacket(const TracePacket&, ProducerID producer);

  // Replaces this object's instances with read-only clones from |src|.
  // If any clone fails, clears all instances from this object.
  void CloneReadOnlyFrom(const TraceBufferProtoVms& src);

  bool HasProducer(ProducerID) const;
  size_t GetMemoryUsageBytes() const;

  bool empty() const { return vms_.empty(); }
  const std::vector<Vm>& instances() const { return vms_; }

 private:
  std::vector<Vm> vms_;

  // Contiguous storage for a packet's slices, as required by ProtoVM's
  // protozero decoder. We reuse it across packets and only allocate when
  // it needs to grow.
  std::string patch_;
};

}  // namespace perfetto

#endif  // SRC_TRACING_SERVICE_TRACE_BUFFER_PROTOVMS_H_
