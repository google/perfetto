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

#include "src/trace_processor/core/exec/context.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::exec {
struct Context::Held {
  struct Bytes {
    void* where;
    // A copy of the bytes as they were when the state was made.
    void* made;
    uint32_t size;
  };
  struct Block {
    void* data;
    uint64_t bytes;
    // The RunVector it was last grown for.
    RunHeader* header;
    uint32_t width;
    RunKeep keep;
    // Where the saved copy of its RunVector's header lives in `bytes.made`,
    // if there is one (may be unaligned on 32-bit platforms; accessed via
    // memcpy).
    void* made;
  };
  struct Buffers {
    ColumnBuffer* data;
    uint32_t count;
  };

  // The state and its size.
  void* state;
  uint32_t size;
  // Whether the state is a HeldState, whose members end their own runs.
  bool members = false;
  // Whether a RunVector grew since the state's last run ended, and whether
  // the state has made or taken batches or buffers.
  bool grew = false;
  bool batches = false;
  // The state's own bytes, if they are restored, then those of its run spans.
  std::vector<Bytes> bytes;
  // The blocks the state's RunVectors use, and blocks kept from earlier runs
  // for any of them to grow into.
  std::vector<Block> blocks;
  std::vector<Block> kept;
  // How many of the first `blocks` need more work at the end of a run than
  // restoring the state's values.
  size_t busy = 0;
  std::vector<std::unique_ptr<RowBatch>> made_batches;
  std::vector<std::unique_ptr<RowBatch>> taken_batches;
  std::vector<std::unique_ptr<RowBatch>> free_batches;
  std::vector<Buffers> made_buffers;
  std::vector<Buffers> taken_buffers;
  std::vector<Buffers> free_buffers;
  // Pipelines whose runs end when this state's run ends.
  std::vector<uint32_t> pipelines;
};

namespace {

// Blocks use Slab's alignment, so a kept block can be reused for any type.
constexpr size_t kBlockAlignment = Slab<uint8_t>::kAlignment;

// Each batch kept for a state to take again costs a few KB, so only this many
// are kept.
constexpr size_t kKeptBatches = kRunKeptBytes / sizeof(RowBatch);

RunHeader ReadMade(const void* made) {
  RunHeader header;
  memcpy(&header, made, sizeof(header));
  return header;
}

void WriteMade(void* made, const RunHeader& header) {
  memcpy(made, &header, sizeof(header));
}

}  // namespace

Context::Context() = default;

Context::~Context() {
  for (Held& held : held_) {
    for (const Held::Block& block : held.blocks) {
      base::AlignedFree(block.data);
    }
    for (const Held::Block& block : held.kept) {
      base::AlignedFree(block.data);
    }
    for (auto* list :
         {&held.made_buffers, &held.taken_buffers, &held.free_buffers}) {
      for (const Held::Buffers& buffers : *list) {
        delete[] buffers.data;
      }
    }
  }
  held_.clear();
  batches_.clear();
  buffer_arrays_.clear();
  for (const Owned& owned : owned_) {
    owned.destroy(owned.object);
  }
  nulls_ = ColumnBuffer();
}

const ColumnBuffer& Context::NullBuffer() {
  if (PERFETTO_UNLIKELY(!nulls_)) {
    // Shared by columns of every type, so as wide as the widest.
    nulls_ = TakeBuffer(ColumnChunk::kMaxValueSize);
    ColumnChunk& chunk = nulls_.chunk();
    memset(chunk.Values<uint8_t>(), 0,
           kMaxBatchRows * ColumnChunk::kMaxValueSize);
    chunk.validity = BitVector::CreateWithSize(kMaxBatchRows, false);
  }
  return nulls_;
}

void Context::SetPipelines(const Pipelines* pipelines) {
  PERFETTO_CHECK(!slots_);
  pipelines_ = pipelines;
  slot_count_ = pipelines ? pipelines->size() : 0;
  slots_ = std::make_unique<Slot[]>(slot_count_);
}

Context::Slot& Context::Start(uint32_t id) {
  PERFETTO_CHECK(id < slot_count_);
  Slot& slot = slots_[id];
  const Pipelines::Entry& entry = (*pipelines_)[id];
  if (!slot.pipeline) {
    slot.pipeline = entry.pipeline->MakeState(*this);
    if (entry.sink) {
      slot.state = entry.sink->MakeSinkState(*this);
    }
  } else {
    entry.pipeline->Rewind(*slot.pipeline);
  }
  slot.running = true;
  return slot;
}

OperatorState* Context::BuildSink(uint32_t id) {
  if (slots_[id].built) {
    return slots_[id].state;
  }
  Slot& slot = Start(id);
  const Pipelines::Entry& entry = (*pipelines_)[id];
  const Pipeline& pipeline = *entry.pipeline;
  const Sink& sink = *entry.sink;
  bool ok = true;
  while (RowBatch* batch = pipeline.Next(*slot.pipeline)) {
    if (!sink.ConsumeBatch(*batch, *slot.state)) {
      ok = false;
      break;
    }
  }
  if (ok && failed()) {
    ok = false;
  }
  if (ok && !sink.FinalizeSink(*slot.state)) {
    ok = false;
  }
  if (!ok) {
    if (!failed()) {
      Fail(base::ErrStatus("sink failed without saying why"));
    }
    EndPipelineRun(id);
    return nullptr;
  }
  slot.built = true;
  return slot.state;
}

OperatorState* Context::IfBuilt(uint32_t id) const {
  if (id >= slot_count_) {
    return nullptr;
  }
  return slots_[id].built ? slots_[id].state : nullptr;
}

RowBatch* Context::NextBatch(uint32_t id) {
  Slot& slot = slots_[id].running ? slots_[id] : Start(id);
  const Pipelines::Entry& entry = (*pipelines_)[id];
  return entry.pipeline->Next(*slot.pipeline);
}

void Context::EndPipelineRun(uint32_t id) {
  if (id >= slot_count_) {
    return;
  }
  Slot& slot = slots_[id];
  if (!slot.running) {
    return;
  }
  slot.running = false;
  slot.built = false;
  (*pipelines_)[id].pipeline->EndRun(*slot.pipeline);
  if (slot.state) {
    EndRun(*slot.state);
  }
}

void Context::UsePipeline(OperatorState& owner, uint32_t id) {
  held_[owner.id_].pipelines.push_back(id);
  SetHolds(owner, true);
}

void Context::Fail(base::Status status) {
  if (error_.ok()) {
    error_ = std::move(status);
  }
}

void Context::AddState(OperatorState* state, uint32_t size, bool restored) {
  state->context_ = this;
  state->id_ = static_cast<uint32_t>(held_.size());
  Held& held = held_.emplace_back();
  held.state = state;
  held.size = size;
  if (restored) {
    // The fields of OperatorState itself never change, so they are not saved.
    static_assert(sizeof(OperatorState) ==
                  sizeof(void*) + 2 * sizeof(uint32_t));
    AddRunBytes(*state, reinterpret_cast<char*>(state) + sizeof(OperatorState),
                size - static_cast<uint32_t>(sizeof(OperatorState)));
  }
}

void Context::AddHeldState(OperatorState* state) {
  held_[state->id_].members = true;
}

void* Context::AllocateSpan(size_t size, size_t align) {
  return states_.Allocate(size,
                          size >= kBlockAlignment ? kBlockAlignment : align);
}

void Context::AddRunBytes(OperatorState& owner, void* where, uint32_t size) {
  if (size) {
    held_[owner.id_].bytes.push_back({where, nullptr, size});
  }
}

void Context::AddOwned(void* object, void (*destroy)(void*)) {
  owned_.push_back({object, destroy});
}

OperatorState* Context::Made(OperatorState* state) {
  Held& held = held_[state->id_];
  for (Held::Bytes& bytes : held.bytes) {
    bytes.made = states_.Allocate(bytes.size, alignof(std::max_align_t));
    memcpy(bytes.made, bytes.where, bytes.size);
  }
  SetHolds(*state, Holds(held, *state));
  return state;
}

void Context::SetHolds(OperatorState& state, bool holds) {
  holding_ += static_cast<uint32_t>(holds) - state.holds_;
  state.holds_ = holds;
}

bool Context::Holds(const Held& held, OperatorState& state) {
  return !held.bytes.empty() || !held.blocks.empty() ||
         !held.made_batches.empty() || !held.made_buffers.empty() ||
         !held.pipelines.empty() ||
         (held.members && static_cast<HeldState&>(state).holds());
}

namespace {

// The header a RunVector which keeps its block has once its run ends.
RunHeader Ended(const RunHeader& header, RunKeep keep) {
  return {header.data, keep == RunKeep::kSmall ? 0 : header.size,
          header.capacity};
}

}  // namespace

void Context::EndHeldRun(OperatorState& state) {
  Held& held = held_[state.id_];
  for (uint32_t id : held.pipelines) {
    EndPipelineRun(id);
  }
  if (held.members) {
    static_cast<HeldState&>(state).EndRun();
  }
  if (!held.blocks.empty()) {
    EndBlocks(held);
  }
  if (held.batches) {
    EndBatches(held);
  }
  for (const Held::Bytes& bytes : held.bytes) {
    memcpy(bytes.where, bytes.made, bytes.size);
  }
  SetHolds(state, Holds(held, state));
}

void Context::EndBatches(Held& held) {
  for (auto& batch : held.made_batches) {
    batch->Reset();
  }
  for (auto& batch : held.taken_batches) {
    batch->Reset();
    if (held.free_batches.size() < kKeptBatches) {
      held.free_batches.push_back(std::move(batch));
    }
  }
  held.taken_batches.clear();
  for (const Held::Buffers& buffers : held.made_buffers) {
    for (uint32_t i = 0; i < buffers.count; ++i) {
      buffers.data[i] = ColumnBuffer();
    }
  }
  for (const Held::Buffers& buffers : held.taken_buffers) {
    for (uint32_t i = 0; i < buffers.count; ++i) {
      buffers.data[i] = ColumnBuffer();
    }
    if (held.free_buffers.size() < kKeptBatches) {
      held.free_buffers.push_back(buffers);
    } else {
      delete[] buffers.data;
    }
  }
  held.taken_buffers.clear();
  held.batches = !held.made_batches.empty() || !held.made_buffers.empty();
}

void Context::EndBlocks(Held& held) {
  if (!held.grew) {
    // No block has moved since the last run which grew one, and the restored
    // values already record where each block is. Only the first `busy` blocks
    // need more work.
    for (size_t i = 0; i < held.busy; ++i) {
      const Held::Block& block = held.blocks[i];
      if (!block.made) {
        *block.header = Ended(*block.header, block.keep);
      }
      if (block.keep == RunKeep::kAllZeroed) {
        RunHeader ended = block.made ? ReadMade(block.made) : *block.header;
        memset(block.data, 0, uint64_t{ended.size} * block.width);
      }
    }
    return;
  }
  // Free the kept blocks which no RunVector grew into during this run, so what
  // is kept never exceeds what one run held.
  for (const Held::Block& block : held.kept) {
    base::AlignedFree(block.data);
  }
  held.kept.clear();
  held.grew = false;
  // A RunVector keeps its block if it is still where it last grew: in the
  // state, in a run span, or in a list kept whole (RunKeep::kAll). One which
  // was moved or swapped since loses its block. When the state's values are
  // restored, the RunVector's end-of-run header is written into the saved copy
  // it is restored from.
  auto state = reinterpret_cast<uintptr_t>(held.state);
  std::vector<uint8_t> placed(held.blocks.size(), 0);
  bool lists = false;
  for (size_t i = 0; i < held.blocks.size(); ++i) {
    Held::Block& block = held.blocks[i];
    auto at = reinterpret_cast<uintptr_t>(block.header);
    block.made = nullptr;
    for (const Held::Bytes& bytes : held.bytes) {
      auto from = reinterpret_cast<uintptr_t>(bytes.where);
      if (at >= from && at < from + bytes.size) {
        block.made = static_cast<char*>(bytes.made) + (at - from);
        break;
      }
    }
    bool small = block.keep != RunKeep::kSmall || block.bytes <= kRunKeptBytes;
    bool keep_in_place =
        small && (block.made || (at >= state && at < state + held.size)) &&
        block.header->data == block.data;
    placed[i] = static_cast<uint8_t>(keep_in_place);
    lists |= keep_in_place && block.keep == RunKeep::kAll;
  }
  // A RunVector whose saved copy points at a block it no longer keeps is
  // restored empty.
  for (size_t i = 0; i < held.blocks.size(); ++i) {
    const Held::Block& block = held.blocks[i];
    if (!placed[i] && block.made) {
      WriteMade(block.made, RunHeader{nullptr, 0, 0});
    }
  }
  for (size_t i = 0; i < held.blocks.size(); ++i) {
    const Held::Block& block = held.blocks[i];
    if (placed[i]) {
      RunHeader ended = Ended(*block.header, block.keep);
      if (block.made) {
        WriteMade(block.made, ended);
      } else {
        *block.header = ended;
      }
    }
  }
  if (lists) {
    EndListedBlocks(held, &placed);
  }
  size_t live = 0;
  for (size_t i = 0; i < held.blocks.size(); ++i) {
    const Held::Block& block = held.blocks[i];
    if (placed[i]) {
      if (block.keep == RunKeep::kAllZeroed) {
        memset(block.data, 0, uint64_t{block.header->size} * block.width);
      }
      held.blocks[live++] = block;
    } else if (block.keep != RunKeep::kSmall || block.bytes <= kRunKeptBytes) {
      held.kept.push_back(block);
    } else {
      base::AlignedFree(block.data);
    }
  }
  held.blocks.resize(live);
  // Move the blocks which need more than restoring values to the front.
  held.busy = 0;
  for (Held::Block& block : held.blocks) {
    if (!block.made || block.keep == RunKeep::kAllZeroed) {
      std::swap(block, held.blocks[held.busy++]);
    }
  }
}

void Context::EndListedBlocks(Held& held, std::vector<uint8_t>* placed) {
  base::SmallVector<size_t, 4> lists;
  for (size_t i = 0; i < held.blocks.size(); ++i) {
    if ((*placed)[i] && held.blocks[i].keep == RunKeep::kAll) {
      lists.emplace_back(i);
    }
  }
  for (size_t l = 0; l < lists.size(); ++l) {
    const Held::Block& outer = held.blocks[lists[l]];
    auto from = reinterpret_cast<uintptr_t>(outer.data);
    for (size_t i = 0; i < held.blocks.size(); ++i) {
      Held::Block& inner = held.blocks[i];
      auto at = reinterpret_cast<uintptr_t>(inner.header);
      if ((*placed)[i] || at < from || at >= from + outer.bytes ||
          inner.header->data != inner.data) {
        continue;
      }
      if (inner.keep == RunKeep::kSmall && inner.bytes > kRunKeptBytes) {
        // The block is freed, so the RunVector, which stays, must not point at
        // it.
        *inner.header = RunHeader{nullptr, 0, 0};
        continue;
      }
      (*placed)[i] = 1;
      *inner.header = Ended(*inner.header, inner.keep);
      if (inner.keep == RunKeep::kAll) {
        lists.emplace_back(i);
      }
    }
  }
}

void Context::GrowRun(OperatorState& owner,
                      RunHeader* header,
                      uint64_t min_capacity,
                      uint32_t width,
                      RunKeep keep) {
  Held& held = held_[owner.id_];
  SetHolds(owner, true);
  held.grew = true;
  uint64_t bytes =
      internal::GrownCapacity(header->capacity, min_capacity) * width;
  // The smallest block kept which is big enough, if any.
  size_t best = held.kept.size();
  for (size_t i = 0; i < held.kept.size(); ++i) {
    if (held.kept[i].bytes >= bytes &&
        (best == held.kept.size() ||
         held.kept[i].bytes < held.kept[best].bytes)) {
      best = i;
    }
  }
  Held::Block grown;
  if (best != held.kept.size()) {
    grown = held.kept[best];
    held.kept[best] = held.kept.back();
    held.kept.pop_back();
  } else {
    grown = {base::AlignedAlloc(kBlockAlignment, bytes),
             bytes,
             nullptr,
             0,
             keep,
             nullptr};
  }
  PERFETTO_CHECK(grown.bytes / width <= UINT32_MAX);
  grown.header = header;
  grown.width = width;
  grown.keep = keep;
  grown.made = nullptr;
  if (!header->data) {
    held.blocks.push_back(grown);
  } else {
    memcpy(grown.data, header->data, uint64_t{header->size} * width);
    [[maybe_unused]] bool found = false;
    for (Held::Block& block : held.blocks) {
      if (block.data != header->data) {
        continue;
      }
      // If a swapped RunVector grew this block, clear the old slot's saved
      // header so EndHeldRun does not restore the freed pointer.
      if (block.made) {
        WriteMade(block.made, RunHeader{nullptr, 0, 0});
      }
      // Freed straight away, as a FlexVector's old allocation would be.
      base::AlignedFree(block.data);
      block = grown;
      found = true;
      break;
    }
    PERFETTO_DCHECK(found);
  }
  header->data = grown.data;
  header->capacity = grown.bytes / width;
}

RowBatch* Context::NewBatch() {
  return batches_.emplace_back(std::make_unique<RowBatch>()).get();
}
RowBatch* Context::NewBatch(OperatorState& owner) {
  Held& held = held_[owner.id_];
  held.batches = true;
  SetHolds(owner, true);
  return held.made_batches.emplace_back(std::make_unique<RowBatch>()).get();
}

RowBatch* Context::TakeBatch(OperatorState& owner) {
  Held& held = held_[owner.id_];
  held.batches = true;
  SetHolds(owner, true);
  if (held.free_batches.empty()) {
    return held.taken_batches.emplace_back(std::make_unique<RowBatch>()).get();
  }
  held.taken_batches.push_back(std::move(held.free_batches.back()));
  held.free_batches.pop_back();
  return held.taken_batches.back().get();
}

ColumnBuffer* Context::NewBuffers(uint32_t count) {
  return buffer_arrays_.emplace_back(new ColumnBuffer[count]).get();
}

ColumnBuffer* Context::NewBuffers(OperatorState& owner, uint32_t count) {
  auto* buffers = new ColumnBuffer[count];
  held_[owner.id_].made_buffers.push_back({buffers, count});
  held_[owner.id_].batches = true;
  SetHolds(owner, true);
  return buffers;
}

ColumnBuffer* Context::TakeBuffers(OperatorState& owner, uint32_t count) {
  Held& held = held_[owner.id_];
  held.batches = true;
  SetHolds(owner, true);
  ColumnBuffer* buffers = nullptr;
  for (size_t i = 0; i < held.free_buffers.size(); ++i) {
    if (held.free_buffers[i].count == count) {
      buffers = held.free_buffers[i].data;
      held.free_buffers[i] = held.free_buffers.back();
      held.free_buffers.pop_back();
      break;
    }
  }
  if (!buffers) {
    buffers = new ColumnBuffer[count];
  }
  held.taken_buffers.push_back({buffers, count});
  return buffers;
}
}  // namespace perfetto::trace_processor::core::exec
