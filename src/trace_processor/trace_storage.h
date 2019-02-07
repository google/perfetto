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

#ifndef SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_
#define SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_

#include <array>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/optional.h"
#include "perfetto/base/string_view.h"
#include "perfetto/base/utils.h"
#include "src/trace_processor/ftrace_utils.h"
#include "src/trace_processor/stats.h"

namespace perfetto {
namespace trace_processor {

// UniquePid is an offset into |unique_processes_|. This is necessary because
// Unix pids are reused and thus not guaranteed to be unique over a long
// period of time.
using UniquePid = uint32_t;

// UniqueTid is an offset into |unique_threads_|. Necessary because tids can
// be reused.
using UniqueTid = uint32_t;

// StringId is an offset into |string_pool_|.
using StringId = uint32_t;

// Identifiers for all the tables in the database.
enum TableId : uint8_t {
  // Intentionally don't have TableId == 0 so that RowId == 0 can refer to an
  // invalid row id.
  kCounters = 1,
  kRawEvents = 2,
  kInstants = 3,
  kSched = 4,
};

// The top 8 bits are set to the TableId and the bottom 32 to the row of the
// table.
using RowId = int64_t;
static const RowId kInvalidRowId = 0;

using ArgSetId = uint32_t;
static const ArgSetId kInvalidArgSetId = 0;

enum RefType {
  kRefNoRef = 0,
  kRefUtid = 1,
  kRefCpuId = 2,
  kRefIrq = 3,
  kRefSoftIrq = 4,
  kRefUpid = 5,
  kRefUtidLookupUpid = 6,
  kRefMax
};

// Stores a data inside a trace file in a columnar form. This makes it efficient
// to read or search across a single field of the trace (e.g. all the thread
// names for a given CPU).
class TraceStorage {
 public:
  TraceStorage();
  TraceStorage(const TraceStorage&) = delete;

  virtual ~TraceStorage();

  // Information about a unique process seen in a trace.
  struct Process {
    explicit Process(uint32_t p) : pid(p) {}
    int64_t start_ns = 0;
    int64_t end_ns = 0;
    StringId name_id = 0;
    uint32_t pid = 0;
  };

  // Information about a unique thread seen in a trace.
  struct Thread {
    explicit Thread(uint32_t t) : tid(t) {}
    int64_t start_ns = 0;
    int64_t end_ns = 0;
    StringId name_id = 0;
    base::Optional<UniquePid> upid;
    uint32_t tid = 0;
  };

  // Generic key value storage which can be referenced by other tables.
  class Args {
   public:
    // Variadic type representing the possible values for the args table.
    struct Variadic {
      enum Type { kInt, kString, kReal };

      static Variadic Integer(int64_t int_value) {
        Variadic variadic;
        variadic.type = Type::kInt;
        variadic.int_value = int_value;
        return variadic;
      }

      static Variadic String(StringId string_id) {
        Variadic variadic;
        variadic.type = Type::kString;
        variadic.string_value = string_id;
        return variadic;
      }

      static Variadic Real(double real_value) {
        Variadic variadic;
        variadic.type = Type::kReal;
        variadic.real_value = real_value;
        return variadic;
      }

      Type type;
      union {
        int64_t int_value;
        StringId string_value;
        double real_value;
      };
    };

    struct Arg {
      StringId flat_key = 0;
      StringId key = 0;
      Variadic value = Variadic::Integer(0);

      // This is only used by the arg tracker and so is not part of the hash.
      RowId row_id = 0;
    };

    struct ArgHasher {
      uint64_t operator()(const Arg& arg) const noexcept {
        uint64_t hash = kFnv1a64OffsetBasis;
        hash ^= static_cast<decltype(hash)>(arg.key);
        hash *= 1099511628211;  // FNV-1a-64 prime.
        // We don't hash arg.flat_key because it's a subsequence of arg.key.
        switch (arg.value.type) {
          case Variadic::Type::kInt:
            hash ^= static_cast<uint64_t>(arg.value.int_value);
            break;
          case Variadic::Type::kString:
            hash ^= static_cast<uint64_t>(arg.value.string_value);
            break;
          case Variadic::Type::kReal:
            hash ^= static_cast<uint64_t>(arg.value.real_value);
            break;
        }
        hash *= kFnv1a64Prime;
        return hash;
      }
    };

    const std::deque<ArgSetId>& set_ids() const { return set_ids_; }
    const std::deque<StringId>& flat_keys() const { return flat_keys_; }
    const std::deque<StringId>& keys() const { return keys_; }
    const std::deque<Variadic>& arg_values() const { return arg_values_; }
    uint32_t args_count() const {
      return static_cast<uint32_t>(set_ids_.size());
    }

    ArgSetId AddArgSet(const std::vector<Arg>& args,
                       uint32_t begin,
                       uint32_t end) {
      ArgSetHash hash = kFnv1a64OffsetBasis;
      for (uint32_t i = begin; i < end; i++) {
        hash ^= ArgHasher()(args[i]);
        hash *= kFnv1a64Prime;
      }

      auto it = arg_row_for_hash_.find(hash);
      if (it != arg_row_for_hash_.end()) {
        return set_ids_[it->second];
      }

      // The +1 ensures that nothing has an id == kInvalidArgSetId == 0.
      ArgSetId id = static_cast<uint32_t>(arg_row_for_hash_.size()) + 1;
      arg_row_for_hash_.emplace(hash, args_count());
      for (uint32_t i = begin; i < end; i++) {
        const auto& arg = args[i];
        set_ids_.emplace_back(id);
        flat_keys_.emplace_back(arg.flat_key);
        keys_.emplace_back(arg.key);
        arg_values_.emplace_back(arg.value);
      }
      return id;
    }

   private:
    using ArgSetHash = uint64_t;

    static constexpr uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325;
    static constexpr uint64_t kFnv1a64Prime = 0xcbf29ce484222325;

    std::deque<ArgSetId> set_ids_;
    std::deque<StringId> flat_keys_;
    std::deque<StringId> keys_;
    std::deque<Variadic> arg_values_;

    std::unordered_map<ArgSetHash, uint32_t> arg_row_for_hash_;
  };

  class Slices {
   public:
    inline size_t AddSlice(uint32_t cpu,
                           int64_t start_ns,
                           int64_t duration_ns,
                           UniqueTid utid,
                           ftrace_utils::TaskState end_state,
                           int32_t priority) {
      cpus_.emplace_back(cpu);
      start_ns_.emplace_back(start_ns);
      durations_.emplace_back(duration_ns);
      utids_.emplace_back(utid);
      end_states_.emplace_back(end_state);
      priorities_.emplace_back(priority);

      if (utid >= rows_for_utids_.size())
        rows_for_utids_.resize(utid + 1);
      rows_for_utids_[utid].emplace_back(slice_count() - 1);
      return slice_count() - 1;
    }

    void set_duration(size_t index, int64_t duration_ns) {
      durations_[index] = duration_ns;
    }

    void set_end_state(size_t index, ftrace_utils::TaskState end_state) {
      end_states_[index] = end_state;
    }

    size_t slice_count() const { return start_ns_.size(); }

    const std::deque<uint32_t>& cpus() const { return cpus_; }

    const std::deque<int64_t>& start_ns() const { return start_ns_; }

    const std::deque<int64_t>& durations() const { return durations_; }

    const std::deque<UniqueTid>& utids() const { return utids_; }

    const std::deque<ftrace_utils::TaskState>& end_state() const {
      return end_states_;
    }

    const std::deque<int32_t>& priorities() const { return priorities_; }

    const std::deque<std::vector<uint32_t>>& rows_for_utids() const {
      return rows_for_utids_;
    }

   private:
    // Each deque below has the same number of entries (the number of slices
    // in the trace for the CPU).
    std::deque<uint32_t> cpus_;
    std::deque<int64_t> start_ns_;
    std::deque<int64_t> durations_;
    std::deque<UniqueTid> utids_;
    std::deque<ftrace_utils::TaskState> end_states_;
    std::deque<int32_t> priorities_;

    // One row per utid.
    std::deque<std::vector<uint32_t>> rows_for_utids_;
  };

  class NestableSlices {
   public:
    inline size_t AddSlice(int64_t start_ns,
                           int64_t duration_ns,
                           UniqueTid utid,
                           StringId cat,
                           StringId name,
                           uint8_t depth,
                           int64_t stack_id,
                           int64_t parent_stack_id) {
      start_ns_.emplace_back(start_ns);
      durations_.emplace_back(duration_ns);
      utids_.emplace_back(utid);
      cats_.emplace_back(cat);
      names_.emplace_back(name);
      depths_.emplace_back(depth);
      stack_ids_.emplace_back(stack_id);
      parent_stack_ids_.emplace_back(parent_stack_id);
      return slice_count() - 1;
    }

    void set_duration(size_t index, int64_t duration_ns) {
      durations_[index] = duration_ns;
    }

    void set_stack_id(size_t index, int64_t stack_id) {
      stack_ids_[index] = stack_id;
    }

    size_t slice_count() const { return start_ns_.size(); }
    const std::deque<int64_t>& start_ns() const { return start_ns_; }
    const std::deque<int64_t>& durations() const { return durations_; }
    const std::deque<UniqueTid>& utids() const { return utids_; }
    const std::deque<StringId>& cats() const { return cats_; }
    const std::deque<StringId>& names() const { return names_; }
    const std::deque<uint8_t>& depths() const { return depths_; }
    const std::deque<int64_t>& stack_ids() const { return stack_ids_; }
    const std::deque<int64_t>& parent_stack_ids() const {
      return parent_stack_ids_;
    }

   private:
    std::deque<int64_t> start_ns_;
    std::deque<int64_t> durations_;
    std::deque<UniqueTid> utids_;
    std::deque<StringId> cats_;
    std::deque<StringId> names_;
    std::deque<uint8_t> depths_;
    std::deque<int64_t> stack_ids_;
    std::deque<int64_t> parent_stack_ids_;
  };

  class Counters {
   public:
    inline size_t AddCounter(int64_t timestamp,
                             StringId name_id,
                             double value,
                             int64_t ref,
                             RefType type) {
      timestamps_.emplace_back(timestamp);
      name_ids_.emplace_back(name_id);
      values_.emplace_back(value);
      refs_.emplace_back(ref);
      types_.emplace_back(type);
      arg_set_ids_.emplace_back(kInvalidArgSetId);
      return counter_count() - 1;
    }

    void set_arg_set_id(uint32_t row, ArgSetId id) { arg_set_ids_[row] = id; }

    size_t counter_count() const { return timestamps_.size(); }

    const std::deque<int64_t>& timestamps() const { return timestamps_; }

    const std::deque<StringId>& name_ids() const { return name_ids_; }

    const std::deque<double>& values() const { return values_; }

    const std::deque<int64_t>& refs() const { return refs_; }

    const std::deque<RefType>& types() const { return types_; }

    const std::deque<ArgSetId>& arg_set_ids() const { return arg_set_ids_; }

   private:
    std::deque<int64_t> timestamps_;
    std::deque<StringId> name_ids_;
    std::deque<double> values_;
    std::deque<int64_t> refs_;
    std::deque<RefType> types_;
    std::deque<ArgSetId> arg_set_ids_;
  };

  class SqlStats {
   public:
    static constexpr size_t kMaxLogEntries = 100;
    void RecordQueryBegin(const std::string& query,
                          int64_t time_queued,
                          int64_t time_started);
    void RecordQueryEnd(int64_t time_ended);
    size_t size() const { return queries_.size(); }
    const std::deque<std::string>& queries() const { return queries_; }
    const std::deque<int64_t>& times_queued() const { return times_queued_; }
    const std::deque<int64_t>& times_started() const { return times_started_; }
    const std::deque<int64_t>& times_ended() const { return times_ended_; }

   private:
    std::deque<std::string> queries_;
    std::deque<int64_t> times_queued_;
    std::deque<int64_t> times_started_;
    std::deque<int64_t> times_ended_;
  };

  class Instants {
   public:
    inline uint32_t AddInstantEvent(int64_t timestamp,
                                    StringId name_id,
                                    double value,
                                    int64_t ref,
                                    RefType type) {
      timestamps_.emplace_back(timestamp);
      name_ids_.emplace_back(name_id);
      values_.emplace_back(value);
      refs_.emplace_back(ref);
      types_.emplace_back(type);
      arg_set_ids_.emplace_back(kInvalidArgSetId);
      return static_cast<uint32_t>(instant_count() - 1);
    }

    void set_arg_set_id(uint32_t row, ArgSetId id) { arg_set_ids_[row] = id; }

    size_t instant_count() const { return timestamps_.size(); }

    const std::deque<int64_t>& timestamps() const { return timestamps_; }

    const std::deque<StringId>& name_ids() const { return name_ids_; }

    const std::deque<double>& values() const { return values_; }

    const std::deque<int64_t>& refs() const { return refs_; }

    const std::deque<RefType>& types() const { return types_; }

    const std::deque<ArgSetId>& arg_set_ids() const { return arg_set_ids_; }

   private:
    std::deque<int64_t> timestamps_;
    std::deque<StringId> name_ids_;
    std::deque<double> values_;
    std::deque<int64_t> refs_;
    std::deque<RefType> types_;
    std::deque<ArgSetId> arg_set_ids_;
  };

  class RawEvents {
   public:
    inline RowId AddRawEvent(int64_t timestamp,
                             StringId name_id,
                             uint32_t cpu,
                             UniqueTid utid) {
      timestamps_.emplace_back(timestamp);
      name_ids_.emplace_back(name_id);
      cpus_.emplace_back(cpu);
      utids_.emplace_back(utid);
      arg_set_ids_.emplace_back(kInvalidArgSetId);
      return CreateRowId(TableId::kRawEvents,
                         static_cast<uint32_t>(raw_event_count() - 1));
    }

    void set_arg_set_id(uint32_t row, ArgSetId id) { arg_set_ids_[row] = id; }

    size_t raw_event_count() const { return timestamps_.size(); }

    const std::deque<int64_t>& timestamps() const { return timestamps_; }

    const std::deque<StringId>& name_ids() const { return name_ids_; }

    const std::deque<uint32_t>& cpus() const { return cpus_; }

    const std::deque<UniqueTid>& utids() const { return utids_; }

    const std::deque<ArgSetId>& arg_set_ids() const { return arg_set_ids_; }

   private:
    std::deque<int64_t> timestamps_;
    std::deque<StringId> name_ids_;
    std::deque<uint32_t> cpus_;
    std::deque<UniqueTid> utids_;
    std::deque<ArgSetId> arg_set_ids_;
  };

  class AndroidLogs {
   public:
    inline size_t AddLogEvent(int64_t timestamp,
                              UniqueTid utid,
                              uint8_t prio,
                              StringId tag_id,
                              StringId msg_id) {
      timestamps_.emplace_back(timestamp);
      utids_.emplace_back(utid);
      prios_.emplace_back(prio);
      tag_ids_.emplace_back(tag_id);
      msg_ids_.emplace_back(msg_id);
      return size() - 1;
    }

    size_t size() const { return timestamps_.size(); }

    const std::deque<int64_t>& timestamps() const { return timestamps_; }
    const std::deque<UniqueTid>& utids() const { return utids_; }
    const std::deque<uint8_t>& prios() const { return prios_; }
    const std::deque<StringId>& tag_ids() const { return tag_ids_; }
    const std::deque<StringId>& msg_ids() const { return msg_ids_; }

   private:
    std::deque<int64_t> timestamps_;
    std::deque<UniqueTid> utids_;
    std::deque<uint8_t> prios_;
    std::deque<StringId> tag_ids_;
    std::deque<StringId> msg_ids_;
  };

  struct Stats {
    using IndexMap = std::map<int, int64_t>;
    int64_t value = 0;
    IndexMap indexed_values;
  };
  using StatsMap = std::array<Stats, stats::kNumKeys>;

  void ResetStorage();

  UniqueTid AddEmptyThread(uint32_t tid) {
    unique_threads_.emplace_back(tid);
    return static_cast<UniqueTid>(unique_threads_.size() - 1);
  }

  UniquePid AddEmptyProcess(uint32_t pid) {
    unique_processes_.emplace_back(pid);
    return static_cast<UniquePid>(unique_processes_.size() - 1);
  }

  // Return an unqiue identifier for the contents of each string.
  // The string is copied internally and can be destroyed after this called.
  // Virtual for testing.
  virtual StringId InternString(base::StringView);

  Process* GetMutableProcess(UniquePid upid) {
    PERFETTO_DCHECK(upid < unique_processes_.size());
    return &unique_processes_[upid];
  }

  Thread* GetMutableThread(UniqueTid utid) {
    PERFETTO_DCHECK(utid < unique_threads_.size());
    return &unique_threads_[utid];
  }

  // Example usage: SetStats(stats::android_log_num_failed, 42);
  void SetStats(size_t key, int64_t value) {
    PERFETTO_DCHECK(key < stats::kNumKeys);
    PERFETTO_DCHECK(stats::kTypes[key] == stats::kSingle);
    stats_[key].value = value;
  }

  // Example usage: IncrementStats(stats::android_log_num_failed, -1);
  void IncrementStats(size_t key, int64_t increment = 1) {
    PERFETTO_DCHECK(key < stats::kNumKeys);
    PERFETTO_DCHECK(stats::kTypes[key] == stats::kSingle);
    stats_[key].value += increment;
  }

  // Example usage: SetIndexedStats(stats::cpu_failure, 1, 42);
  void SetIndexedStats(size_t key, int index, int64_t value) {
    PERFETTO_DCHECK(key < stats::kNumKeys);
    PERFETTO_DCHECK(stats::kTypes[key] == stats::kIndexed);
    stats_[key].indexed_values[index] = value;
  }

  // Reading methods.
  const std::string& GetString(StringId id) const {
    PERFETTO_DCHECK(id < string_pool_.size());
    return string_pool_[id];
  }

  const Process& GetProcess(UniquePid upid) const {
    PERFETTO_DCHECK(upid < unique_processes_.size());
    return unique_processes_[upid];
  }

  const Thread& GetThread(UniqueTid utid) const {
    // Allow utid == 0 for idle thread retrieval.
    PERFETTO_DCHECK(utid < unique_threads_.size());
    return unique_threads_[utid];
  }

  static RowId CreateRowId(TableId table, uint32_t row) {
    return (static_cast<RowId>(table) << kRowIdTableShift) | row;
  }

  static std::pair<int8_t /*table*/, uint32_t /*row*/> ParseRowId(RowId rowid) {
    auto id = static_cast<uint64_t>(rowid);
    auto table_id = static_cast<uint8_t>(id >> kRowIdTableShift);
    auto row = static_cast<uint32_t>(id & ((1ull << kRowIdTableShift) - 1));
    return std::make_pair(table_id, row);
  }

  const Slices& slices() const { return slices_; }
  Slices* mutable_slices() { return &slices_; }

  const NestableSlices& nestable_slices() const { return nestable_slices_; }
  NestableSlices* mutable_nestable_slices() { return &nestable_slices_; }

  const Counters& counters() const { return counters_; }
  Counters* mutable_counters() { return &counters_; }

  const SqlStats& sql_stats() const { return sql_stats_; }
  SqlStats* mutable_sql_stats() { return &sql_stats_; }

  const Instants& instants() const { return instants_; }
  Instants* mutable_instants() { return &instants_; }

  const AndroidLogs& android_logs() const { return android_log_; }
  AndroidLogs* mutable_android_log() { return &android_log_; }

  const StatsMap& stats() const { return stats_; }

  const Args& args() const { return args_; }
  Args* mutable_args() { return &args_; }

  const RawEvents& raw_events() const { return raw_events_; }
  RawEvents* mutable_raw_events() { return &raw_events_; }

  const std::deque<std::string>& string_pool() const { return string_pool_; }

  // |unique_processes_| always contains at least 1 element becuase the 0th ID
  // is reserved to indicate an invalid process.
  size_t process_count() const { return unique_processes_.size(); }

  // |unique_threads_| always contains at least 1 element becuase the 0th ID
  // is reserved to indicate an invalid thread.
  size_t thread_count() const { return unique_threads_.size(); }

  // Number of interned strings in the pool. Includes the empty string w/ ID=0.
  size_t string_count() const { return string_pool_.size(); }

  // Start / end ts (in nanoseconds) across the parsed trace events.
  // Returns (0, 0) if the trace is empty.
  std::pair<int64_t, int64_t> GetTraceTimestampBoundsNs() const;

 private:
  static constexpr uint8_t kRowIdTableShift = 32;

  using StringHash = uint64_t;

  TraceStorage& operator=(const TraceStorage&) = default;

  // Stats about parsing the trace.
  StatsMap stats_{};

  // One entry for each CPU in the trace.
  Slices slices_;

  // Args for all other tables.
  Args args_;

  // One entry for each unique string in the trace.
  std::deque<std::string> string_pool_;

  // One entry for each unique string in the trace.
  std::unordered_map<StringHash, StringId> string_index_;

  // One entry for each UniquePid, with UniquePid as the index.
  std::deque<Process> unique_processes_;

  // One entry for each UniqueTid, with UniqueTid as the index.
  std::deque<Thread> unique_threads_;

  // Slices coming from userspace events (e.g. Chromium TRACE_EVENT macros).
  NestableSlices nestable_slices_;

  // Counter events from the trace. This includes CPU frequency events as well
  // systrace trace_marker counter events.
  Counters counters_;

  SqlStats sql_stats_;

  // These are instantaneous events in the trace. They have no duration
  // and do not have a value that make sense to track over time.
  // e.g. signal events
  Instants instants_;

  // Raw events are every ftrace event in the trace. The raw event includes
  // the timestamp and the pid. The args for the raw event will be in the
  // args table. This table can be used to generate a text version of the
  // trace.
  RawEvents raw_events_;
  AndroidLogs android_log_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_STORAGE_H_
