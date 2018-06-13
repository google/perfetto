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

#ifndef SRC_TRACE_PROCESSOR_SCHED_SLICE_TABLE_H_
#define SRC_TRACE_PROCESSOR_SCHED_SLICE_TABLE_H_

#include <limits>

#include "sqlite3.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class SchedSliceTable {
 public:
  using Constraint = sqlite3_index_info::sqlite3_index_constraint;

  class Cursor {
   public:
    Cursor(SchedSliceTable* table, const TraceStorage* storage);

    int Filter(int idxNum, const char* idxStr, int argc, sqlite3_value** argv);
    int Next();
    int Eof();

    int Column(sqlite3_context* context, int N);
    int RowId(sqlite_int64* pRowid);

   private:
    template <class T>
    class NumericConstraints {
     public:
      bool Initialize(const Constraint& cs, sqlite3_value* value);
      bool Matches(T value) {
        if (value < min_value || (value == min_value && !min_equals)) {
          return false;
        } else if (value > max_value || (value == max_value && !max_equals)) {
          return false;
        }
        return true;
      }

     private:
      T min_value = std::numeric_limits<T>::min();
      bool min_equals = true;
      T max_value = std::numeric_limits<T>::max();
      bool max_equals = true;
    };

    struct PerCpuState {
      size_t index = 0;
    };

    struct FilterState {
      // One entry for each cpu which is used in filtering.
      std::array<PerCpuState, TraceStorage::kMaxCpus> per_cpu_state;
      size_t next_slice_cpu = 0;

      NumericConstraints<uint64_t> timestamp_constraints;
      NumericConstraints<uint32_t> cpu_constraints;
    };

    void FindNextSliceForCpu(uint32_t cpu, size_t start_index);

    void FindNextSliceAmongCpus();

    sqlite3_vtab_cursor base_;  // Must be first.

    SchedSliceTable* const table_;
    const TraceStorage* const storage_;

    FilterState filter_state_;
  };

  SchedSliceTable(const TraceStorage* storage);
  static sqlite3_module CreateModule();

  int BestIndex(sqlite3_index_info* index_info);

  int Open(sqlite3_vtab_cursor** ppCursor);

 private:
  enum Column { kTimestamp = 0, kCpu = 1, kDuration = 2 };

  sqlite3_vtab base_;  // Must be first.
  const TraceStorage* const storage_;

  // This vector contains one outer entry for each xBestIndex call and one
  // inner entry for each constraint provided by that xBestIndex call.
  std::vector<std::vector<Constraint>> indexes_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_SCHED_SLICE_TABLE_H_
