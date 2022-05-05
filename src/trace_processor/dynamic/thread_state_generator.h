/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DYNAMIC_THREAD_STATE_GENERATOR_H_
#define SRC_TRACE_PROCESSOR_DYNAMIC_THREAD_STATE_GENERATOR_H_

#include "src/trace_processor/sqlite/db_sqlite_table.h"

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// Dynamic table implementing the thread state table.
// This table is a basically the same as sched with extra information added
// about wakeups (obtained from sched_waking/sched_wakeup).
class ThreadStateGenerator : public DbSqliteTable::DynamicTableGenerator {
 public:
  explicit ThreadStateGenerator(TraceProcessorContext* context);
  ~ThreadStateGenerator() override;

  Table::Schema CreateSchema() override;
  std::string TableName() override;
  uint32_t EstimateRowCount() override;
  base::Status ValidateConstraints(const QueryConstraints&) override;
  base::Status ComputeTable(const std::vector<Constraint>& cs,
                            const std::vector<Order>& ob,
                            const BitVector& cols_used,
                            std::unique_ptr<Table>& table_return) override;

  // Visible for testing.
  std::unique_ptr<tables::ThreadStateTable> ComputeThreadStateTable();

 private:
  struct ThreadSchedInfo {
    base::Optional<int64_t> desched_ts;
    base::Optional<StringId> desched_end_state;
    base::Optional<uint32_t> scheduled_row;
    base::Optional<bool> io_wait;
    base::Optional<int64_t> runnable_ts;
    base::Optional<StringId> blocked_function;
    base::Optional<UniqueTid> runnable_waker_utid;
  };
  using TidInfoMap = base::FlatHashMap<UniqueTid,
                                       ThreadSchedInfo,
                                       base::AlreadyHashed<UniqueTid>,
                                       base::QuadraticProbe,
                                       /*AppendOnly=*/true>;

  void AddSchedEvent(const Table& sched,
                     uint32_t sched_idx,
                     TidInfoMap& state_map,
                     tables::ThreadStateTable* table);

  void AddWakingEvent(const Table& wakeup,
                      uint32_t wakeup_idx,
                      TidInfoMap& state_map);

  void AddBlockedReasonEvent(const Table& blocked_reason,
                             uint32_t blocked_idx,
                             TidInfoMap& state_map);

  void FlushPendingEventsForThread(UniqueTid utid,
                                   const ThreadSchedInfo&,
                                   tables::ThreadStateTable* table,
                                   base::Optional<int64_t> end_ts);

  std::unique_ptr<tables::ThreadStateTable> unsorted_thread_state_table_;
  base::Optional<Table> sorted_thread_state_table_;

  const StringId running_string_id_;
  const StringId runnable_string_id_;

  TraceProcessorContext* context_ = nullptr;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DYNAMIC_THREAD_STATE_GENERATOR_H_
