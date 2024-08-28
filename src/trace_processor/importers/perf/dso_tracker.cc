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

#include "src/trace_processor/importers/perf/dso_tracker.h"

#include <cstdint>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "protos/third_party/simpleperf/record_file.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::perf_importer {
namespace {

using third_party::simpleperf::proto::pbzero::FileFeature;
using DexFile = FileFeature::DexFile;
using ElfFile = FileFeature::ElfFile;
using KernelModule = FileFeature::KernelModule;
using DsoType = FileFeature::DsoType;
using Symbol = FileFeature::Symbol;

void InsertSymbols(const FileFeature::Decoder& file,
                   AddressRangeMap<std::string>& out) {
  for (auto raw_symbol = file.symbol(); raw_symbol; ++raw_symbol) {
    Symbol::Decoder symbol(*raw_symbol);
    out.TrimOverlapsAndEmplace(
        AddressRange::FromStartAndSize(symbol.vaddr(), symbol.len()),
        symbol.name().ToStdString());
  }
}
}  // namespace

DsoTracker::DsoTracker(TraceProcessorContext* context)
    : context_(context),
      mapping_table_(context_->storage->stack_profile_mapping_table()) {}
DsoTracker::~DsoTracker() = default;

void DsoTracker::AddSimpleperfFile2(const FileFeature::Decoder& file) {
  Dso dso;
  switch (file.type()) {
    case DsoType::DSO_KERNEL:
      InsertSymbols(file, kernel_symbols_);
      return;

    case DsoType::DSO_ELF_FILE: {
      ElfFile::Decoder elf(file.elf_file());
      dso.load_bias = file.min_vaddr() - elf.file_offset_of_min_vaddr();
      break;
    }

    case DsoType::DSO_KERNEL_MODULE: {
      KernelModule::Decoder module(file.kernel_module());
      dso.load_bias = file.min_vaddr() - module.memory_offset_of_min_vaddr();
      break;
    }

    case DsoType::DSO_DEX_FILE:
    case DsoType::DSO_SYMBOL_MAP_FILE:
    case DsoType::DSO_UNKNOWN_FILE:
      return;
  }

  InsertSymbols(file, dso.symbols);
  files_.Insert(context_->storage->InternString(file.path()), std::move(dso));
}

void DsoTracker::SymbolizeFrames() {
  const StringId kEmptyString = context_->storage->InternString("");
  for (auto frame = context_->storage->mutable_stack_profile_frame_table()
                        ->IterateRows();
       frame; ++frame) {
    if (frame.name() != kNullStringId && frame.name() != kEmptyString) {
      continue;
    }

    if (!TrySymbolizeFrame(frame.row_reference())) {
      SymbolizeKernelFrame(frame.row_reference());
    }
  }
}

void DsoTracker::SymbolizeKernelFrame(
    tables::StackProfileFrameTable::RowReference frame) {
  const auto mapping = *mapping_table_.FindById(frame.mapping());
  uint64_t address = static_cast<uint64_t>(frame.rel_pc()) +
                     static_cast<uint64_t>(mapping.start());
  auto symbol = kernel_symbols_.Find(address);
  if (symbol == kernel_symbols_.end()) {
    return;
  }
  frame.set_name(
      context_->storage->InternString(base::StringView(symbol->second)));
}

bool DsoTracker::TrySymbolizeFrame(
    tables::StackProfileFrameTable::RowReference frame) {
  const auto mapping = *mapping_table_.FindById(frame.mapping());
  auto* file = files_.Find(mapping.name());
  if (!file) {
    return false;
  }

  // Load bias is something we can only determine by looking at the actual elf
  // file. Thus PERF_RECORD_MMAP{2} events do not record it. So we need to
  // potentially do an adjustment here if the load_bias tracked in the mapping
  // table and the one reported by the file are mismatched.
  uint64_t adj = file->load_bias - static_cast<uint64_t>(mapping.load_bias());

  auto symbol = file->symbols.Find(static_cast<uint64_t>(frame.rel_pc()) + adj);
  if (symbol == file->symbols.end()) {
    return false;
  }
  frame.set_name(
      context_->storage->InternString(base::StringView(symbol->second)));
  return true;
}

}  // namespace perfetto::trace_processor::perf_importer
