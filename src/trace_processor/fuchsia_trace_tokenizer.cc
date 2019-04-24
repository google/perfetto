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

#include "src/trace_processor/fuchsia_trace_tokenizer.h"

#include <inttypes.h>
#include <unordered_map>

#include "perfetto/base/logging.h"
#include "perfetto/base/string_view.h"
#include "src/trace_processor/ftrace_utils.h"
#include "src/trace_processor/fuchsia_provider_view.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_sorter.h"

namespace perfetto {
namespace trace_processor {

namespace {
// Record types
constexpr uint32_t kMetadata = 0;
constexpr uint32_t kInitialization = 1;
constexpr uint32_t kString = 2;
constexpr uint32_t kThread = 3;
constexpr uint32_t kEvent = 4;
constexpr uint32_t kKernelObject = 7;
constexpr uint32_t kContextSwitch = 8;

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

// Argument types
constexpr uint32_t kArgKernelObject = 8;
}  // namespace

FuchsiaTraceTokenizer::FuchsiaTraceTokenizer(TraceProcessorContext* context)
    : context_(context) {
  RegisterProvider(0, "");
}

FuchsiaTraceTokenizer::~FuchsiaTraceTokenizer() = default;

bool FuchsiaTraceTokenizer::Parse(std::unique_ptr<uint8_t[]> data,
                                  size_t size) {
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
    leftover_bytes_.insert(leftover_bytes_.end(), data.get() + byte_offset,
                           data.get() + size);
    return true;
  }
  if (leftover_bytes_.size() > 0) {
    // There is a record starting from leftover bytes.
    if (leftover_bytes_.size() < 8) {
      // Header was previously incomplete, but we have enough now.
      // Copy bytes into |leftover_bytes_| so that the whole header is present,
      // and update |byte_offset| and |size| accordingly.
      size_t needed_bytes = 8 - leftover_bytes_.size();
      leftover_bytes_.insert(leftover_bytes_.end(), data.get() + byte_offset,
                             data.get() + needed_bytes);
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
      std::unique_ptr<uint8_t[]> buf(new uint8_t[record_len_bytes]);
      memcpy(&buf[0], leftover_bytes_.data(), leftover_bytes_.size());
      memcpy(&buf[leftover_bytes_.size()], &data[byte_offset], missing_bytes);
      byte_offset += missing_bytes;
      size -= missing_bytes;
      leftover_bytes_.clear();

      TraceBlobView leftover_record(std::move(buf), 0, record_len_bytes);
      ParseRecord(std::move(leftover_record));
    } else {
      // There are not enough bytes for the full record. Add all the bytes we
      // have to leftover_bytes_ and wait for more.
      leftover_bytes_.insert(leftover_bytes_.end(), data.get() + byte_offset,
                             data.get() + byte_offset + size);
      return true;
    }
  }

  TraceBlobView full_view(std::move(data), byte_offset, size);

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
    if (record_len_bytes == 0) {
      PERFETTO_DLOG("Unexpected record of size 0");
      return false;
    }

    if (record_offset + record_len_bytes > size)
      break;

    TraceBlobView record =
        full_view.slice(byte_offset + record_offset, record_len_bytes);
    ParseRecord(std::move(record));

    record_offset += record_len_bytes;
  }

  leftover_bytes_.insert(leftover_bytes_.end(),
                         full_view.data() + record_offset,
                         full_view.data() + size);
  return true;
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

  const uint64_t* record = reinterpret_cast<const uint64_t*>(tbv.data());
  uint64_t header = *record;

  uint32_t record_type = fuchsia_trace_utils::ReadField<uint32_t>(header, 0, 3);
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
          std::string name(reinterpret_cast<const char*>(&record[1]), name_len);
          RegisterProvider(provider_id, name);
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
      current_provider_->ticks_per_second = record[1];
      break;
    }
    case kString: {
      uint32_t index = fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 30);
      if (index != 0) {
        uint32_t len = fuchsia_trace_utils::ReadField<uint32_t>(header, 32, 46);
        base::StringView s(reinterpret_cast<const char*>(&record[1]), len);
        StringId id = storage->InternString(s);
        current_provider_->string_table[index] = id;
      }
      break;
    }
    case kThread: {
      uint32_t index = fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 23);
      if (index != 0) {
        fuchsia_trace_utils::ThreadInfo tinfo;
        tinfo.pid = record[1];
        tinfo.tid = record[2];

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

      // Build the minimal FuchsiaProviderView needed by
      // the record. This means the thread information if not inline, and any
      // non-inline strings (name, category for now, arg names and string values
      // in the future.
      const uint64_t* current = &record[1];
      auto provider_view =
          std::unique_ptr<FuchsiaProviderView>(new FuchsiaProviderView());
      provider_view->set_ticks_per_second(current_provider_->ticks_per_second);

      uint64_t ticks = *current++;
      int64_t ts = fuchsia_trace_utils::TicksToNs(
          ticks, current_provider_->ticks_per_second);

      if (fuchsia_trace_utils::IsInlineThread(thread_ref)) {
        // Skip over inline thread
        fuchsia_trace_utils::ReadInlineThread(&current);
      } else {
        provider_view->InsertThread(
            thread_ref, current_provider_->thread_table[thread_ref]);
      }

      if (fuchsia_trace_utils::IsInlineString(cat_ref)) {
        // Skip over inline string
        fuchsia_trace_utils::ReadInlineString(&current, cat_ref);
      } else {
        provider_view->InsertString(cat_ref,
                                    current_provider_->string_table[cat_ref]);
      }

      if (fuchsia_trace_utils::IsInlineString(name_ref)) {
        // Skip over inline string
        fuchsia_trace_utils::ReadInlineString(&current, name_ref);
      } else {
        provider_view->InsertString(name_ref,
                                    current_provider_->string_table[name_ref]);
      }

      sorter->PushFuchsiaRecord(ts, std::move(tbv), std::move(provider_view));

      break;
    }
    case kKernelObject: {
      uint32_t obj_type =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 23);
      uint32_t name_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 24, 39);

      const uint64_t* current = &record[1];
      uint64_t obj_id = *current++;

      StringId name = StringId();
      if (fuchsia_trace_utils::IsInlineString(name_ref)) {
        name = storage->InternString(
            fuchsia_trace_utils::ReadInlineString(&current, name_ref));
      } else {
        name = current_provider_->string_table[name_ref];
      }

      switch (obj_type) {
        case kZxObjTypeProcess: {
          // Note: Fuchsia pid/tids are 64 bits but Perfetto's tables only
          // support 32 bits. This is usually not an issue except for
          // artificial koids which have the 2^63 bit set. This is used for
          // things such as virtual threads.
          procs->UpdateProcess(static_cast<uint32_t>(obj_id),
                               base::Optional<uint32_t>(),
                               base::StringView(storage->GetString(name)));
          break;
        }
        case kZxObjTypeThread: {
          uint32_t n_args =
              fuchsia_trace_utils::ReadField<uint32_t>(header, 40, 43);
          uint64_t pid = 0;

          // Scan for a Kernel Object argument named "process"
          for (uint32_t i = 0; i < n_args; i++) {
            const uint64_t* arg_base = current;
            uint64_t arg_header = *current++;
            uint32_t arg_type =
                fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 0, 3);
            uint32_t arg_size =
                fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 4, 15);
            if (arg_type == kArgKernelObject) {
              uint32_t arg_name_ref =
                  fuchsia_trace_utils::ReadField<uint32_t>(arg_header, 16, 31);
              base::StringView arg_name;
              if (fuchsia_trace_utils::IsInlineString(arg_name_ref)) {
                arg_name = fuchsia_trace_utils::ReadInlineString(&current,
                                                                 arg_name_ref);
              } else {
                arg_name = storage->GetString(
                    current_provider_->string_table[arg_name_ref]);
              }

              if (arg_name == "process") {
                pid = *current++;
              }
            }

            current = arg_base + arg_size;
          }

          pid_table_[obj_id] = pid;

          UniqueTid utid = procs->UpdateThread(static_cast<uint32_t>(obj_id),
                                               static_cast<uint32_t>(pid));
          storage->GetMutableThread(utid)->name_id = name;
          break;
        }
        default: {
          PERFETTO_DLOG("Skipping Kernel Object record with type %d", obj_type);
          break;
        }
      }
      break;
    }
    case kContextSwitch: {
      // Context switch records come in order, so they do not need to go through
      // TraceSorter.
      uint32_t cpu = fuchsia_trace_utils::ReadField<uint32_t>(header, 16, 23);
      uint32_t outgoing_state =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 24, 27);
      uint32_t outgoing_thread_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 28, 35);
      uint32_t incoming_thread_ref =
          fuchsia_trace_utils::ReadField<uint32_t>(header, 36, 43);
      int32_t outgoing_priority =
          fuchsia_trace_utils::ReadField<int32_t>(header, 44, 51);

      uint64_t ticks = record[1];
      int64_t ts = fuchsia_trace_utils::TicksToNs(
          ticks, current_provider_->ticks_per_second);

      const uint64_t* current = &record[2];

      fuchsia_trace_utils::ThreadInfo outgoing_thread;
      if (fuchsia_trace_utils::IsInlineThread(outgoing_thread_ref)) {
        outgoing_thread = fuchsia_trace_utils::ReadInlineThread(&current);
      } else {
        outgoing_thread = current_provider_->thread_table[outgoing_thread_ref];
      }

      fuchsia_trace_utils::ThreadInfo incoming_thread;
      if (fuchsia_trace_utils::IsInlineThread(incoming_thread_ref)) {
        incoming_thread = fuchsia_trace_utils::ReadInlineThread(&current);
      } else {
        incoming_thread = current_provider_->thread_table[incoming_thread_ref];
      }

      // A thread with priority 0 represents an idle CPU
      if (cpu_threads_.count(cpu) != 0 && outgoing_priority != 0) {
        // TODO(bhamrick): Some early events will fail to associate with their
        // pid because the kernel object info event hasn't been processed yet.
        if (pid_table_.count(outgoing_thread.tid) > 0) {
          outgoing_thread.pid = pid_table_[outgoing_thread.tid];
        }

        UniqueTid utid =
            procs->UpdateThread(static_cast<uint32_t>(outgoing_thread.tid),
                                static_cast<uint32_t>(outgoing_thread.pid));
        RunningThread previous_thread = cpu_threads_[cpu];

        ftrace_utils::TaskState end_state;
        switch (outgoing_state) {
          case kThreadNew:
          case kThreadRunning: {
            end_state =
                ftrace_utils::TaskState(ftrace_utils::TaskState::kRunnable);
            break;
          }
          case kThreadBlocked: {
            end_state = ftrace_utils::TaskState(
                ftrace_utils::TaskState::kInterruptibleSleep);
            break;
          }
          case kThreadSuspended: {
            end_state =
                ftrace_utils::TaskState(ftrace_utils::TaskState::kStopped);
            break;
          }
          case kThreadDying: {
            end_state =
                ftrace_utils::TaskState(ftrace_utils::TaskState::kExitZombie);
            break;
          }
          case kThreadDead: {
            end_state =
                ftrace_utils::TaskState(ftrace_utils::TaskState::kExitDead);
            break;
          }
          default: { break; }
        }

        storage->mutable_slices()->AddSlice(cpu, previous_thread.start_ts,
                                            ts - previous_thread.start_ts, utid,
                                            end_state, outgoing_priority);
      }

      RunningThread new_running;
      new_running.info = incoming_thread;
      new_running.start_ts = ts;
      cpu_threads_[cpu] = new_running;
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

}  // namespace trace_processor
}  // namespace perfetto
