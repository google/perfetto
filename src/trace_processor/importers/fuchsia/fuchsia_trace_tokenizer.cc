/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/fuchsia/fuchsia_trace_tokenizer.h"

#include <cinttypes>
#include <limits>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "src/trace_processor/importers/common/cpu_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_record.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_parser.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/types/task_state.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

namespace {

using fuchsia_trace_utils::ArgValue;

// Record types
constexpr uint32_t kMetadata = 0;
constexpr uint32_t kInitialization = 1;
constexpr uint32_t kString = 2;
constexpr uint32_t kThread = 3;
constexpr uint32_t kEvent = 4;
constexpr uint32_t kBlob = 5;
constexpr uint32_t kKernelObject = 7;
constexpr uint32_t kSchedulerEvent = 8;

constexpr uint32_t kSchedulerEventLegacyContextSwitch = 0;
constexpr uint32_t kSchedulerEventContextSwitch = 1;
constexpr uint32_t kSchedulerEventThreadWakeup = 2;

// Metadata types
constexpr uint32_t kProviderInfo = 1;
constexpr uint32_t kProviderSection = 2;
constexpr uint32_t kProviderEvent = 3;

// Thread states
constexpr uint32_t kThreadNew = 0;
constexpr uint32_t kThreadRunning = 1;
constexpr uint32_t kThreadSuspended = 2;
constexpr uint32_t kThreadBlocked = 3;
constexpr uint32_t kThreadDying = 4;
constexpr uint32_t kThreadDead = 5;

// Zircon object types
constexpr uint32_t kZxObjTypeProcess = 1;
constexpr uint32_t kZxObjTypeThread = 2;

constexpr int32_t kIdleWeight = std::numeric_limits<int32_t>::min();

}  // namespace

FuchsiaTraceTokenizer::FuchsiaTraceTokenizer(TraceProcessorContext* context)
    : context_(context),
      proto_reader_(context),
      running_string_id_(context->storage->InternString("Running")),
      runnable_string_id_(context->storage->InternString("R")),
      preempted_string_id_(context->storage->InternString("R+")),
      waking_string_id_(context->storage->InternString("W")),
      blocked_string_id_(context->storage->InternString("S")),
      suspended_string_id_(context->storage->InternString("T")),
      exit_dying_string_id_(context->storage->InternString("Z")),
      exit_dead_string_id_(context->storage->InternString("X")),
      incoming_weight_id_(context->storage->InternString("incoming_weight")),
      outgoing_weight_id_(context->storage->InternString("outgoing_weight")),
      weight_id_(context->storage->InternString("weight")),
      process_id_(context->storage->InternString("process")) {
  RegisterProvider(0, "");
}

FuchsiaTraceTokenizer::~FuchsiaTraceTokenizer() = default;

util::Status FuchsiaTraceTokenizer::Parse(TraceBlobView blob) {
  size_t size = blob.size();

  // The relevant internal state is |leftover_bytes_|. Each call to Parse should
  // maintain the following properties, unless a fatal error occurs in which
  // case it should return false and no assumptions should be made about the
  // resulting internal state:
  //
  // 1) Every byte passed to |Parse| has either been passed to |ParseRecord| or
  // is present in |leftover_bytes_|, but not both.
  // 2) |leftover_bytes_| does not contain a complete record.
  //
  // Parse is responsible for creating the "full" |TraceBlobView|s, which own
  // the underlying data. Generally, there will be one such view. However, if
  // there is a record that started in an earlier call, then a new buffer is
  // created here to make the bytes in that record contiguous.
  //
  // Because some of the bytes in |data| might belong to the record starting in
  // |leftover_bytes_|, we track the offset at which the following record will
  // start.
  size_t byte_offset = 0;

  // Look for a record starting with the leftover bytes.
  if (leftover_bytes_.size() + size < 8) {
    // Even with the new bytes, we can't even read the header of the next
    // record, so just add the new bytes to |leftover_bytes_| and return.
    leftover_bytes_.insert(leftover_bytes_.end(), blob.data() + byte_offset,
                           blob.data() + size);
    return util::OkStatus();
  }
  if (!leftover_bytes_.empty()) {
    // There is a record starting from leftover bytes.
    if (leftover_bytes_.size() < 8) {
      // Header was previously incomplete, but we have enough now.
      // Copy bytes into |leftover_bytes_| so that the whole header is present,
      // and update |byte_offset| and |size| accordingly.
      size_t needed_bytes = 8 - leftover_bytes_.size();
      leftover_bytes_.insert(leftover_bytes_.end(), blob.data() + byte_offset,
                             blob.data() + needed_bytes);
      byte_offset += needed_bytes;
      size -= needed_bytes;
    }
    // Read the record length from the header.
    uint64_t header =
        *reinterpret_cast<const uint64_t*>(leftover_bytes_.data());
    uint32_t record_len_words =
        fuchsia_trace_utils::ReadField<uint32_t>(header, 4, 15);
    uint32_t record_len_bytes = record_len_words * sizeof(uint64_t);

    // From property (2) above, leftover_bytes_ must have had less than a full
    // record to start with. We padded leftover_bytes_ out to read the header,
    // so it may now be a full record (in the case that the record consists of
    // only the header word), but it still cannot have any extra bytes.
    PERFETTO_DCHECK(leftover_bytes_.size() <= record_len_bytes);
    size_t missing_bytes = record_len_bytes - leftover_bytes_.size();

    if (missing_bytes <= size) {
      // We have enough bytes to complete the partial record. Create a new
      // buffer for that record.
      TraceBlob buf = TraceBlob::Allocate(record_len_bytes);
      memcpy(buf.data(), leftover_bytes_.data(), leftover_bytes_.size());
      memcpy(buf.data() + leftover_bytes_.size(), blob.data() + byte_offset,
             missing_bytes);
      byte_offset += missing_bytes;
      size -= missing_bytes;
      leftover_bytes_.clear();
      ParseRecord(TraceBlobView(std::move(buf)));
    } else {
      // There are not enough bytes for the full record. Add all the bytes we
      // have to leftover_bytes_ and wait for more.
      leftover_bytes_.insert(leftover_bytes_.end(), blob.data() + byte_offset,
                             blob.data() + byte_offset + size);
      return util::OkStatus();
    }
  }

  TraceBlobView full_view = blob.slice_off(byte_offset, size);

  // |record_offset| is a number of bytes past |byte_offset| where the record
  // under consideration starts. As a result, it must always be in the range [0,
  // size-8]. Any larger offset means we don't have enough bytes for the header.
  size_t record_offset = 0;
  while (record_offset + 8 <= size) {
    uint64_t header =
        *reinterpret_cast<const uint64_t*>(full_view.data() + record_offset);
    uint32_t record_len_bytes =
        fuchsia_trace_utils::ReadField<uint32_t>(header, 4, 15) *
        sizeof(uint64_t);
    if (record_len_bytes == 0)
      return util::ErrStatus("Unexpected record of size 0");

    if (record_offset + record_len_bytes > size)
      break;

    TraceBlobView record = full_view.slice_off(record_offset, record_len_bytes);
    ParseRecord(std::move(record));

    record_offset += record_len_bytes;
  }

  leftover_bytes_.insert(leftover_bytes_.end(),
                         full_view.data() + record_offset,
                         full_view.data() + size);

  TraceBlob perfetto_blob =
      TraceBlob::CopyFrom(proto_trace_data_.data(), proto_trace_data_.size());
  proto_trace_data_.clear();

  return proto_reader_.Parse(TraceBlobView(std::move(perfetto_blob)));
}

StringId FuchsiaTraceTokenizer::IdForOutgoingThreadState(uint32_t state) {
  switch (state) {
    case kThreadNew:
    case kThreadRunning:
      return runnable_string_id_;
    case kThreadBlocked:
      return blocked_string_id_;
    case kThreadSuspended:
      return suspended_string_id_;
    case kThreadDying:
      return exit_dying_string_id_;
    case kThreadDead:
      return exit_dead_string_id_;
    default:
      return kNullStringId;
  }
}

void FuchsiaTraceTokenizer::SwitchFrom(Thread* thread,
                                       int64_t ts,
                                       uint32_t cpu,
                                       uint32_t thread_state) {
  TraceStorage* storage = context_->storage.get();
  ProcessTracker* procs = context_->process_tracker.get();

  StringId state = IdForOutgoingThreadState(thread_state);
  UniqueTid utid = procs->UpdateThread(static_cast<uint32_t>(thread->info.tid),
                                       static_cast<uint32_t>(thread->info.pid));

  const auto duration = ts - thread->last_ts;
  thread->last_ts = ts;

  // Close the slice record if one is open for this thread.
  if (thread->last_slice_row.has_value()) {
    auto row_ref = thread->last_slice_row->ToRowReference(
        storage->mutable_sched_slice_table());
    row_ref.set_dur(duration);
    row_ref.set_end_state(state);
    thread->last_slice_row.reset();
  }

  // Close the state record if one is open for this thread.
  if (thread->last_state_row.has_value()) {
    auto row_ref = thread->last_state_row->ToRowReference(
        storage->mutable_thread_state_table());
    row_ref.set_dur(duration);
    thread->last_state_row.reset();
  }

  // Open a new state record to track the duration of the outgoing
  // state.
  tables::ThreadStateTable::Row state_row;
  state_row.ts = ts;
  state_row.ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  state_row.dur = -1;
  state_row.state = state;
  state_row.utid = utid;
  auto state_row_number =
      storage->mutable_thread_state_table()->Insert(state_row).row_number;
  thread->last_state_row = state_row_number;
}

void FuchsiaTraceTokenizer::SwitchTo(Thread* thread,
                                     int64_t ts,
                                     uint32_t cpu,
                                     int32_t weight) {
  TraceStorage* storage = context_->storage.get();
  ProcessTracker* procs = context_->process_tracker.get();

  UniqueTid utid = procs->UpdateThread(static_cast<uint32_t>(thread->info.tid),
                                       static_cast<uint32_t>(thread->info.pid));

  const auto duration = ts - thread->last_ts;
  thread->last_ts = ts;

  // Close the state record if one is open for this thread.
  if (thread->last_state_row.has_value()) {
    auto row_ref = thread->last_state_row->ToRowReference(
        storage->mutable_thread_state_table());
    row_ref.set_dur(duration);
    thread->last_state_row.reset();
  }

  auto ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  // Open a new slice record for this thread.
  tables::SchedSliceTable::Row slice_row;
  slice_row.ts = ts;
  slice_row.ucpu = ucpu;
  slice_row.dur = -1;
  slice_row.utid = utid;
  slice_row.priority = weight;
  auto slice_row_number =
      storage->mutable_sched_slice_table()->Insert(slice_row).row_number;
  thread->last_slice_row = slice_row_number;

  // Open a new state record for this thread.
  tables::ThreadStateTable::Row state_row;
  state_row.ts = ts;
  state_row.ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  state_row.dur = -1;
  state_row.state = running_string_id_;
  state_row.utid = utid;
  auto state_row_number =
      storage->mutable_thread_state_table()->Insert(state_row).row_number;
  thread->last_state_row = state_row_number;
}

void FuchsiaTraceTokenizer::Wake(Thread* thread, int64_t ts, uint32_t cpu) {
  TraceStorage* storage = context_->storage.get();
  ProcessTracker* procs = context_->process_tracker.get();

  UniqueTid utid = procs->UpdateThread(static_cast<uint32_t>(thread->info.tid),
                                       static_cast<uint32_t>(thread->info.pid));

  const auto duration = ts - thread->last_ts;
  thread->last_ts = ts;

  // Close the state record if one is open for this thread.
  if (thread->last_state_row.has_value()) {
    auto row_ref = thread->last_state_row->ToRowReference(
        storage->mutable_thread_state_table());
    row_ref.set_dur(duration);
    thread->last_state_row.reset();
  }

  // Open a new state record for this thread.
  tables::ThreadStateTable::Row state_row;
  state_row.ts = ts;
  state_row.ucpu = context_->cpu_tracker->GetOrCreateCpu(cpu);
  state_row.dur = -1;
  state_row.state = waking_string_id_;
  state_row.utid = utid;
  auto state_row_number =
      storage->mutable_thread_state_table()->Insert(state_row).row_number;
  thread->last_state_row = state_row_number;
}

// Most record types are read and recorded in |TraceStorage| here directly.
// Event records are sorted by timestamp before processing, so instead of
// recording them in |TraceStorage| they are given to |TraceSorter|. In order to
// facilitate the parsing after sorting, a small view of the provider's string
// and thread tables is passed alongside the record. See |FuchsiaProviderView|.
void FuchsiaTraceTokenizer::ParseRecord(TraceBlobView tbv) {
  TraceStorage* storage = context_->storage.get();
  ProcessTracker* procs = context_->process_tracker.get();
  TraceSorter* sorter = context_->sorter.get();

  fuchsia_trace_utils::RecordCursor cursor(tbv.data(), tbv.length());
  uint64_t header;
  if (!cursor.ReadUint64(&header)) {
    context_->storage->IncrementStats(stats::fuchsia_invalid_event);
    return;
  }

  uint32_t record_type = fuchsia_trace_utils::ReadField<uint32_t>(header, 0, 3);

  // All non-metadata events require current_provider_ to be set.
  if (record_type != kMetadata && current_provider_ == nullptr) {
    context_->storage->IncrementStats(stats::fuchsia_invalid_event);
    return;
  }

  // Adapters for FuchsiaTraceParser::ParseArgs.
  const auto intern_string = [this](base::StringView string) {
    return context_->storage->InternString(string);
  };
  const auto get_string = [this](uint16_t index) {
    return current_provider_->GetString(index);
  };

  switch (record_type) {
    case kMetadata: {
      uint32_t metadata_type =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 19);
      switch (metadata_type) {
        case kProviderInfo: {
          uint32_t provider_id =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 20, 51);
          uint32_t name_len =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 52, 59);
          base::StringView name_view;
          if (!cursor.ReadInlineString(name_len, &name_view)) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }
          RegisterProvider(provider_id, name_view.ToStdString());
          break;
        }
        case kProviderSection: {
          uint32_t provider_id =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 20, 51);
          current_provider_ = providers_[provider_id].get();
          break;
        }
        case kProviderEvent: {
          // TODO(bhamrick): Handle buffer fill events
          PERFETTO_DLOG(
              "Ignoring provider event. Events may have been dropped");
          break;
        }
      }
      break;
    }
    case kInitialization: {
      if (!cursor.ReadUint64(&current_provider_->ticks_per_second)) {
        context_->storage->IncrementStats(stats::fuchsia_invalid_event);
        return;
      }
      break;
    }
    case kString: {
      uint32_t index = fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 30);
      if (index != 0) {
        uint32_t len = fuchsia_trace_utils::ReadField<uint32_t>(header, 32, 46);
        base::StringView s;
        if (!cursor.ReadInlineString(len, &s)) {
          context_->storage->IncrementStats(stats::fuchsia_invalid_event);
          return;
        }
        StringId id = storage->InternString(s);

        current_provider_->string_table[index] = id;
      }
      break;
    }
    case kThread: {
      uint32_t index = fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 23);
      if (index != 0) {
        FuchsiaThreadInfo tinfo;
        if (!cursor.ReadInlineThread(&tinfo)) {
          context_->storage->IncrementStats(stats::fuchsia_invalid_event);
          return;
        }

        current_provider_->thread_table[index] = tinfo;
      }
      break;
    }
    case kEvent: {
      uint32_t thread_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 24, 31);
      uint32_t cat_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 32, 47);
      uint32_t name_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 48, 63);

      // Build the FuchsiaRecord for the event, i.e. extract the thread
      // information if not inline, and any non-inline strings (name, category
      // for now, arg names and string values in the future).
      FuchsiaRecord record(std::move(tbv));
      record.set_ticks_per_second(current_provider_->ticks_per_second);

      uint64_t ticks;
      if (!cursor.ReadUint64(&ticks)) {
        context_->storage->IncrementStats(stats::fuchsia_invalid_event);
        return;
      }
      int64_t ts = fuchsia_trace_utils::TicksToNs(
          ticks, current_provider_->ticks_per_second);
      if (ts < 0) {
        storage->IncrementStats(stats::fuchsia_timestamp_overflow);
        return;
      }

      if (fuchsia_trace_utils::IsInlineThread(thread_ref)) {
        // Skip over inline thread
        cursor.ReadInlineThread(nullptr);
      } else {
        record.InsertThread(thread_ref,
                            current_provider_->GetThread(thread_ref));
      }

      if (fuchsia_trace_utils::IsInlineString(cat_ref)) {
        // Skip over inline string
        cursor.ReadInlineString(cat_ref, nullptr);
      } else {
        record.InsertString(cat_ref, current_provider_->GetString(cat_ref));
      }

      if (fuchsia_trace_utils::IsInlineString(name_ref)) {
        // Skip over inline string
        cursor.ReadInlineString(name_ref, nullptr);
      } else {
        record.InsertString(name_ref, current_provider_->GetString(name_ref));
      }

      uint32_t n_args =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 20, 23);
      for (uint32_t i = 0; i < n_args; i++) {
        const size_t arg_base = cursor.WordIndex();
        uint64_t arg_header;
        if (!cursor.ReadUint64(&arg_header)) {
          storage->IncrementStats(stats::fuchsia_invalid_event);
          return;
        }
        uint32_t arg_type =
            fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 0, 3);
        uint32_t arg_size_words =
            fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 4, 15);
        uint32_t arg_name_ref =
            fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 16, 31);

        if (fuchsia_trace_utils::IsInlineString(arg_name_ref)) {
          // Skip over inline string
          cursor.ReadInlineString(arg_name_ref, nullptr);
        } else {
          record.InsertString(arg_name_ref,
                              current_provider_->GetString(arg_name_ref));
        }

        if (arg_type == ArgValue::ArgType::kString) {
          uint32_t arg_value_ref =
              fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 32, 47);
          if (fuchsia_trace_utils::IsInlineString(arg_value_ref)) {
            // Skip over inline string
            cursor.ReadInlineString(arg_value_ref, nullptr);
          } else {
            record.InsertString(arg_value_ref,
                                current_provider_->GetString(arg_value_ref));
          }
        }

        cursor.SetWordIndex(arg_base + arg_size_words);
      }

      sorter->PushFuchsiaRecord(ts, std::move(record));
      break;
    }
    case kBlob: {
      constexpr uint32_t kPerfettoBlob = 3;
      uint32_t blob_type =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 48, 55);
      if (blob_type == kPerfettoBlob) {
        FuchsiaRecord record(std::move(tbv));
        uint32_t blob_size =
            fuchsia_trace_utils::ReadField<uint32_t>(header, 32, 46);
        uint32_t name_ref =
            fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 31);

        // We don't need the name, but we still need to parse it in case it is
        // inline
        if (fuchsia_trace_utils::IsInlineString(name_ref)) {
          base::StringView name_view;
          if (!cursor.ReadInlineString(name_ref, &name_view)) {
            storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }
        }

        // Append the Blob into the embedded perfetto bytes -- we'll parse them
        // all after the main pass is done.
        if (!cursor.ReadBlob(blob_size, proto_trace_data_)) {
          storage->IncrementStats(stats::fuchsia_invalid_event);
          return;
        }
      }
      break;
    }
    case kKernelObject: {
      uint32_t obj_type =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 23);
      uint32_t name_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 24, 39);

      uint64_t obj_id;
      if (!cursor.ReadUint64(&obj_id)) {
        storage->IncrementStats(stats::fuchsia_invalid_event);
        return;
      }

      StringId name = StringId();
      if (fuchsia_trace_utils::IsInlineString(name_ref)) {
        base::StringView name_view;
        if (!cursor.ReadInlineString(name_ref, &name_view)) {
          storage->IncrementStats(stats::fuchsia_invalid_event);
          return;
        }
        name = storage->InternString(name_view);
      } else {
        name = current_provider_->GetString(name_ref);
      }

      switch (obj_type) {
        case kZxObjTypeProcess: {
          // Note: Fuchsia pid/tids are 64 bits but Perfetto's tables only
          // support 32 bits. This is usually not an issue except for
          // artificial koids which have the 2^63 bit set. This is used for
          // things such as virtual threads.
          procs->SetProcessMetadata(
              static_cast<uint32_t>(obj_id), std::optional<uint32_t>(),
              base::StringView(storage->GetString(name)), base::StringView());
          break;
        }
        case kZxObjTypeThread: {
          uint32_t n_args =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 40, 43);

          auto maybe_args = FuchsiaTraceParser::ParseArgs(
              cursor, n_args, intern_string, get_string);
          if (!maybe_args.has_value()) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }

          uint64_t pid = 0;
          for (const auto arg : *maybe_args) {
            if (arg.name == process_id_) {
              if (arg.value.Type() != ArgValue::ArgType::kKoid) {
                storage->IncrementStats(stats::fuchsia_invalid_event);
                return;
              }
              pid = arg.value.Koid();
            }
          }

          Thread& thread = GetThread(obj_id);
          thread.info.pid = pid;

          UniqueTid utid = procs->UpdateThread(static_cast<uint32_t>(obj_id),
                                               static_cast<uint32_t>(pid));
          auto& tt = *storage->mutable_thread_table();
          tt[utid].set_name(name);
          break;
        }
        default: {
          PERFETTO_DLOG("Skipping Kernel Object record with type %d", obj_type);
          break;
        }
      }
      break;
    }
    case kSchedulerEvent: {
      // Context switch records come in order, so they do not need to go through
      // TraceSorter.
      uint32_t event_type =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 60, 63);
      switch (event_type) {
        case kSchedulerEventLegacyContextSwitch: {
          uint32_t cpu =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 23);
          uint32_t outgoing_state =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 24, 27);
          uint32_t outgoing_thread_ref =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 28, 35);
          int32_t outgoing_priority =
              fuchsia_trace_utils::ReadField<int32_t>(header, 44, 51);
          uint32_t incoming_thread_ref =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 36, 43);
          int32_t incoming_priority =
              fuchsia_trace_utils::ReadField<int32_t>(header, 52, 59);

          int64_t ts;
          if (!cursor.ReadTimestamp(current_provider_->ticks_per_second, &ts)) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }
          if (ts == -1) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }

          FuchsiaThreadInfo outgoing_thread_info;
          if (fuchsia_trace_utils::IsInlineThread(outgoing_thread_ref)) {
            if (!cursor.ReadInlineThread(&outgoing_thread_info)) {
              context_->storage->IncrementStats(stats::fuchsia_invalid_event);
              return;
            }
          } else {
            outgoing_thread_info =
                current_provider_->GetThread(outgoing_thread_ref);
          }
          Thread& outgoing_thread = GetThread(outgoing_thread_info.tid);

          FuchsiaThreadInfo incoming_thread_info;
          if (fuchsia_trace_utils::IsInlineThread(incoming_thread_ref)) {
            if (!cursor.ReadInlineThread(&incoming_thread_info)) {
              context_->storage->IncrementStats(stats::fuchsia_invalid_event);
              return;
            }
          } else {
            incoming_thread_info =
                current_provider_->GetThread(incoming_thread_ref);
          }
          Thread& incoming_thread = GetThread(incoming_thread_info.tid);

          // Idle threads are identified by pid == 0 and prio == 0.
          const bool incoming_is_idle =
              incoming_thread.info.pid == 0 && incoming_priority == 0;
          const bool outgoing_is_idle =
              outgoing_thread.info.pid == 0 && outgoing_priority == 0;

          // Handle switching away from the currently running thread.
          if (!outgoing_is_idle) {
            SwitchFrom(&outgoing_thread, ts, cpu, outgoing_state);
          }

          // Handle switching to the new currently running thread.
          if (!incoming_is_idle) {
            SwitchTo(&incoming_thread, ts, cpu, incoming_priority);
          }
          break;
        }
        case kSchedulerEventContextSwitch: {
          const uint32_t argument_count =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 19);
          const uint32_t cpu =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 20, 35);
          const uint32_t outgoing_state =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 36, 39);

          int64_t ts;
          if (!cursor.ReadTimestamp(current_provider_->ticks_per_second, &ts)) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }
          if (ts < 0) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }

          uint64_t outgoing_tid;
          if (!cursor.ReadUint64(&outgoing_tid)) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }
          Thread& outgoing_thread = GetThread(outgoing_tid);

          uint64_t incoming_tid;
          if (!cursor.ReadUint64(&incoming_tid)) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }
          Thread& incoming_thread = GetThread(incoming_tid);

          auto maybe_args = FuchsiaTraceParser::ParseArgs(
              cursor, argument_count, intern_string, get_string);
          if (!maybe_args.has_value()) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }

          int32_t incoming_weight = 0;
          int32_t outgoing_weight = 0;

          for (const auto& arg : *maybe_args) {
            if (arg.name == incoming_weight_id_) {
              if (arg.value.Type() != ArgValue::ArgType::kInt32) {
                context_->storage->IncrementStats(stats::fuchsia_invalid_event);
                return;
              }
              incoming_weight = arg.value.Int32();
            } else if (arg.name == outgoing_weight_id_) {
              if (arg.value.Type() != ArgValue::ArgType::kInt32) {
                context_->storage->IncrementStats(stats::fuchsia_invalid_event);
                return;
              }
              outgoing_weight = arg.value.Int32();
            }
          }

          const bool incoming_is_idle = incoming_weight == kIdleWeight;
          const bool outgoing_is_idle = outgoing_weight == kIdleWeight;

          // Handle switching away from the currently running thread.
          if (!outgoing_is_idle) {
            SwitchFrom(&outgoing_thread, ts, cpu, outgoing_state);
          }

          // Handle switching to the new currently running thread.
          if (!incoming_is_idle) {
            SwitchTo(&incoming_thread, ts, cpu, incoming_weight);
          }
          break;
        }
        case kSchedulerEventThreadWakeup: {
          const uint32_t argument_count =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 19);
          const uint32_t cpu =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 20, 35);

          int64_t ts;
          if (!cursor.ReadTimestamp(current_provider_->ticks_per_second, &ts)) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }
          if (ts < 0) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }

          uint64_t waking_tid;
          if (!cursor.ReadUint64(&waking_tid)) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }
          Thread& waking_thread = GetThread(waking_tid);

          auto maybe_args = FuchsiaTraceParser::ParseArgs(
              cursor, argument_count, intern_string, get_string);
          if (!maybe_args.has_value()) {
            context_->storage->IncrementStats(stats::fuchsia_invalid_event);
            return;
          }

          int32_t waking_weight = 0;

          for (const auto& arg : *maybe_args) {
            if (arg.name == weight_id_) {
              if (arg.value.Type() != ArgValue::ArgType::kInt32) {
                context_->storage->IncrementStats(stats::fuchsia_invalid_event);
                return;
              }
              waking_weight = arg.value.Int32();
            }
          }

          const bool waking_is_idle = waking_weight == kIdleWeight;
          if (!waking_is_idle) {
            Wake(&waking_thread, ts, cpu);
          }
          break;
        }
        default:
          PERFETTO_DLOG("Skipping unknown scheduler event type %d", event_type);
          break;
      }

      break;
    }
    default: {
      PERFETTO_DLOG("Skipping record of unknown type %d", record_type);
      break;
    }
  }
}

void FuchsiaTraceTokenizer::RegisterProvider(uint32_t provider_id,
                                             std::string name) {
  std::unique_ptr<ProviderInfo> provider(new ProviderInfo());
  provider->name = name;
  current_provider_ = provider.get();
  providers_[provider_id] = std::move(provider);
}

base::Status FuchsiaTraceTokenizer::NotifyEndOfFile() {
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
