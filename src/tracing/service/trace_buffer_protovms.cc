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

#include "src/tracing/service/trace_buffer_protovms.h"

#include <algorithm>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/protozero/field.h"
#include "src/protovm/vm.h"

namespace perfetto {

TraceBufferProtoVms::Vm::Vm() = default;
TraceBufferProtoVms::Vm::~Vm() = default;
TraceBufferProtoVms::Vm::Vm(Vm&&) noexcept = default;

TraceBufferProtoVms::Vm TraceBufferProtoVms::Vm::CloneReadOnly() const {
  Vm cloned_vm;
  cloned_vm.data_source_name = data_source_name;
  cloned_vm.program_hash = program_hash;
  cloned_vm.memory_limit_kb = memory_limit_kb;
  cloned_vm.producers = producers;
  cloned_vm.instance = instance->CloneReadOnly();
  return cloned_vm;
}

TraceBufferProtoVms::TraceBufferProtoVms() = default;
TraceBufferProtoVms::~TraceBufferProtoVms() = default;

void TraceBufferProtoVms::MaybeSetUp(const std::string& data_source_name,
                                     const std::string& program_bytes,
                                     uint32_t memory_limit_kb,
                                     ProducerID producer_id) {
  Vm* vm = nullptr;
  // Re-use existing ProtoVM instance, if any.
  uint64_t program_hash = base::MurmurHashValue(program_bytes);
  auto vm_it = std::find_if(vms_.begin(), vms_.end(), [&](const Vm& other) {
    return other.data_source_name == data_source_name &&
           other.program_hash == program_hash;
  });
  if (vm_it != vms_.end()) {
    vm = &(*vm_it);
  } else {
    // Otherwise instantiate new ProtoVM
    Vm new_vm;
    new_vm.data_source_name = data_source_name;
    new_vm.program_hash = program_hash;
    new_vm.memory_limit_kb = memory_limit_kb;

    protozero::ConstBytes program_bytes_view{
        reinterpret_cast<const uint8_t*>(program_bytes.data()),
        program_bytes.size()};

    new_vm.instance = std::make_unique<protovm::Vm>(
        program_bytes_view, new_vm.memory_limit_kb * 1024);
    if (!new_vm.instance) {
      PERFETTO_ELOG("Failed to allocate ProtoVM");
      return;
    }

    vms_.push_back(std::move(new_vm));
    vm = &vms_.back();
  }
  // Update ProtoVM's producer IDs
  vm->producers.insert(producer_id);
}

void TraceBufferProtoVms::MaybeProcessOverwrittenPacket(
    const TracePacket& packet,
    ProducerID producer) {
  patch_.clear();

  for (auto& vm : vms_) {
    if (vm.producers.find(producer) == vm.producers.end()) {
      continue;
    }
    // TODO(keanmariotti): add an optimized path for the case
    // "packet.slices().size() == 1" (zero copy)
    if (patch_.empty()) {
      packet.GetRawBytes(&patch_);
    }
    protozero::ConstBytes bytes{reinterpret_cast<const uint8_t*>(patch_.data()),
                                patch_.size()};
    auto status = vm.instance->ApplyPatch(bytes);
    if (status.IsOk()) {
      break;
    }
    if (status.IsAbort()) {
      // TODO(keanmariotti): consider doing something more here. Ideally
      // triggering a field upload containing the stacktrace.
      PERFETTO_ELOG("ProtoVM abort while applying patch. Stacktrace:\n%s",
                    base::Join(status.stacktrace(), "\n").c_str());
    }
  }
}

void TraceBufferProtoVms::CloneReadOnlyFrom(const TraceBufferProtoVms& src) {
  vms_.clear();
  for (const auto& vm : src.vms_) {
    auto vm_cloned = vm.CloneReadOnly();
    if (!vm_cloned.instance) {
      PERFETTO_ELOG("Failed to clone ProtoVMs");
      vms_.clear();
      break;
    }
    vms_.push_back(std::move(vm_cloned));
  }
}

bool TraceBufferProtoVms::HasProducer(ProducerID producer) const {
  return std::any_of(vms_.begin(), vms_.end(), [&](const Vm& vm) {
    return vm.producers.count(producer) != 0;
  });
}

size_t TraceBufferProtoVms::GetMemoryUsageBytes() const {
  size_t total_bytes = 0;
  for (const Vm& vm : vms_)
    total_bytes += vm.instance->GetMemoryUsageBytes();
  return total_bytes;
}

}  // namespace perfetto
