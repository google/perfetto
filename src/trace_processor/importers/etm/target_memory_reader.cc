/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/etm/target_memory_reader.h"

#include "src/trace_processor/importers/etm/opencsd.h"

namespace perfetto::trace_processor::etm {

ocsd_err_t TargetMemoryReader::ReadTargetMemory(const ocsd_vaddr_t,
                                                const uint8_t,
                                                const ocsd_mem_space_acc_t,
                                                uint32_t* num_bytes,
                                                uint8_t*) {
  *num_bytes = 0;
  return OCSD_OK;
}

void TargetMemoryReader::InvalidateMemAccCache(uint8_t) {}

void TargetMemoryReader::SetTs(std::optional<int64_t>) {}
void TargetMemoryReader::SetPeContext(const ocsd_pe_context&) {}

Mapping* TargetMemoryReader::FindMapping(const AddressRange&) const {
  return nullptr;
}

}  // namespace perfetto::trace_processor::etm
