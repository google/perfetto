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

#include "src/trace_processor/perfetto_sql/pipeline/row_parking.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/parked_rows.h"
#include "src/trace_processor/core/exec/pipeline.h"

namespace perfetto::trace_processor::pipeline {
namespace {

namespace ex = core::exec;
using Step = ex::Pipeline::Step;
using Keeps = ex::StepColumns::Keeps;

ex::StepColumns Describe(Step& step) {
  ex::StepColumns columns;
  if (auto* transform = std::get_if<std::unique_ptr<ex::Transform>>(&step)) {
    (*transform)->DescribeColumns(&columns);
    PERFETTO_DCHECK(columns.keeps == Keeps::kAll);
  } else {
    std::get<std::unique_ptr<ex::Operator>>(step)->DescribeColumns(&columns);
  }
  return columns;
}

// Where a value is, at some point in the rewritten steps.
struct Location {
  enum class Kind : uint8_t { kDropped, kInBatch, kParked };

  static Location InBatch(uint32_t position) {
    return {Kind::kInBatch, position, 0, 0};
  }
  static Location Parked(uint32_t store, uint32_t column) {
    return {Kind::kParked, 0, store, column};
  }

  Kind kind = Kind::kDropped;
  // For kInBatch: its position in the batch.
  uint32_t position = 0;
  // For kParked: which store, and which of its columns.
  uint32_t store = 0;
  uint32_t column = 0;
};

class Rewriter {
 public:
  Rewriter(uint32_t source_columns,
           std::vector<uint32_t>* outputs,
           uint32_t* next_store,
           std::vector<Step>* steps)
      : source_columns_(source_columns),
        outputs_(outputs),
        next_store_(next_store),
        steps_(steps) {}

  base::Status Run();

 private:
  struct Store {
    // Context::Store(slot).
    uint32_t slot;
    // Where the rows' numbers in the store are.
    Location numbers;
  };

  // By step, which planned columns are read after it, and how many columns
  // it outputs as planned.
  void FindLiveColumns(const std::vector<ex::StepColumns>& columns);

  // Where `value` is in the batch, fetching it back first if it is parked.
  uint32_t Require(Location* value);

  // Adds the FetchRows being built, if any.
  void AddFetch();

  // Parks the values read after step `step`, before it, and gives it the
  // rows' numbers.
  void ParkBefore(size_t step, uint32_t* numbers_position);

  uint32_t source_columns_;
  std::vector<uint32_t>* outputs_;
  uint32_t* next_store_;
  std::vector<Step>* steps_;

  // As planned: by step, the columns it outputs, and which of them are read
  // after it.
  std::vector<uint32_t> planned_widths_;
  std::vector<std::vector<bool>> live_after_;

  // While rewriting: by planned column, where its value is.
  std::vector<Location> planned_;
  std::vector<Store> stores_;
  uint32_t width_ = 0;
  std::optional<ex::FetchRowsSpec> fetch_;
  std::vector<Step> out_;
  base::Status status_ = base::OkStatus();
};

void Rewriter::FindLiveColumns(const std::vector<ex::StepColumns>& columns) {
  size_t count = columns.size();
  planned_widths_.resize(count + 1);
  planned_widths_[0] = source_columns_;
  for (size_t i = 0; i < count; ++i) {
    const ex::StepColumns& step = columns[i];
    planned_widths_[i + 1] = step.keeps == Keeps::kNone
                                 ? step.added
                                 : planned_widths_[i] + step.added;
  }
  std::vector<bool> live(planned_widths_[count], outputs_ == nullptr);
  if (outputs_) {
    for (uint32_t column : *outputs_) {
      live[column] = true;
    }
  }
  live_after_.resize(count);
  for (size_t i = count; i-- > 0;) {
    live_after_[i] = live;
    const ex::StepColumns& step = columns[i];
    if (step.keeps == Keeps::kNone) {
      live.assign(planned_widths_[i], false);
    } else {
      live.resize(planned_widths_[i]);
    }
    for (const uint32_t* read : step.reads) {
      live[*read] = true;
    }
  }
}

uint32_t Rewriter::Require(Location* value) {
  switch (value->kind) {
    case Location::Kind::kInBatch:
      return value->position;
    case Location::Kind::kDropped:
      status_ = base::ErrStatus(
          "pipeline: a column is read after the rows were reordered, but was "
          "not kept");
      return 0;
    case Location::Kind::kParked:
      break;
  }
  uint32_t column = value->column;
  uint32_t slot = stores_[value->store].slot;
  uint32_t row_column = Require(&stores_[value->store].numbers);
  if (fetch_ && (fetch_->slot != slot || fetch_->row_column != row_column)) {
    AddFetch();
  }
  if (!fetch_) {
    fetch_ = ex::FetchRowsSpec{slot, row_column, {}};
  }
  fetch_->columns.push_back(column);
  *value = Location::InBatch(width_++);
  return value->position;
}

void Rewriter::AddFetch() {
  if (!fetch_) {
    return;
  }
  out_.emplace_back(std::make_unique<ex::FetchRows>(std::move(*fetch_)));
  fetch_.reset();
}

void Rewriter::ParkBefore(size_t step, uint32_t* numbers_position) {
  AddFetch();
  const std::vector<bool>& live = live_after_[step];
  auto store = static_cast<uint32_t>(stores_.size());
  ex::ParkRowsSpec park;
  park.slot = (*next_store_)++;
  // The values which go in the store, in its order.
  std::vector<Location*> parked;
  for (uint32_t column = 0; column < planned_widths_[step]; ++column) {
    Location& value = planned_[column];
    if (value.kind == Location::Kind::kInBatch && live[column]) {
      park.columns.push_back(value.position);
      parked.push_back(&value);
    }
  }
  // A store is needed while a value read later is parked in it, or the
  // numbers of another store which is needed.
  std::vector<bool> needed(stores_.size(), false);
  for (uint32_t column = 0; column < planned_widths_[step]; ++column) {
    const Location& value = planned_[column];
    if (value.kind == Location::Kind::kParked && live[column]) {
      needed[value.store] = true;
    }
  }
  for (size_t s = 0; s < stores_.size(); ++s) {
    const Location& numbers = stores_[s].numbers;
    // A store's numbers are only ever parked in a later store.
    if (needed[s] && numbers.kind == Location::Kind::kParked) {
      needed[numbers.store] = true;
    }
  }
  for (size_t s = 0; s < stores_.size(); ++s) {
    Location& numbers = stores_[s].numbers;
    if (needed[s] && numbers.kind == Location::Kind::kInBatch) {
      park.columns.push_back(numbers.position);
      parked.push_back(&numbers);
    }
  }
  out_.emplace_back(std::make_unique<ex::ParkRows>(park));
  *numbers_position = width_++;

  // The reorder outputs the rows' numbers alone: everything else in the
  // batch is either in the new store or not read again.
  for (Location& value : planned_) {
    if (value.kind == Location::Kind::kInBatch) {
      value = Location();
    }
  }
  for (Store& other : stores_) {
    if (other.numbers.kind == Location::Kind::kInBatch) {
      other.numbers = Location();
    }
  }
  for (uint32_t i = 0; i < parked.size(); ++i) {
    *parked[i] = Location::Parked(store, i);
  }
  stores_.push_back({park.slot, Location::InBatch(0)});
  width_ = 1;
}

base::Status Rewriter::Run() {
  std::vector<ex::StepColumns> columns;
  bool reorders = false;
  for (Step& step : *steps_) {
    columns.push_back(Describe(step));
    reorders |= columns.back().keeps == Keeps::kRowNumbers;
  }
  if (!reorders) {
    return base::OkStatus();
  }
  FindLiveColumns(columns);

  width_ = source_columns_;
  for (uint32_t column = 0; column < source_columns_; ++column) {
    planned_.push_back(Location::InBatch(column));
  }
  for (size_t i = 0; i < steps_->size(); ++i) {
    ex::StepColumns& step = columns[i];
    for (uint32_t* read : step.reads) {
      *read = Require(&planned_[*read]);
    }
    if (step.keeps == Keeps::kRowNumbers) {
      ParkBefore(i, step.numbers);
    } else {
      AddFetch();
    }
    out_.push_back(std::move((*steps_)[i]));
    if (step.keeps == Keeps::kNone) {
      // Nothing from before is in the batch any more.
      planned_.assign(0, Location());
      stores_.clear();
      width_ = 0;
    }
    planned_.resize(planned_widths_[i + 1]);
    for (uint32_t added = 0; added < step.added; ++added) {
      planned_[planned_widths_[i + 1] - step.added + added] =
          Location::InBatch(width_++);
    }
    RETURN_IF_ERROR(status_);
  }

  if (outputs_) {
    // The columns read after the pipeline are told where they now are.
    for (uint32_t& column : *outputs_) {
      column = Require(&planned_[column]);
    }
    AddFetch();
    RETURN_IF_ERROR(status_);
  } else {
    // Any column may be read after the pipeline, where it was planned.
    uint32_t planned_width = planned_widths_.back();
    std::vector<uint32_t> layout(planned_width);
    bool moved = false;
    for (uint32_t column = 0; column < planned_width; ++column) {
      layout[column] = Require(&planned_[column]);
      moved |= layout[column] != column;
    }
    AddFetch();
    RETURN_IF_ERROR(status_);
    if (moved) {
      out_.emplace_back(std::make_unique<ex::SelectColumns>(std::move(layout)));
    }
  }
  *steps_ = std::move(out_);
  return base::OkStatus();
}

}  // namespace

base::Status ParkReorderedRows(uint32_t source_columns,
                               std::vector<uint32_t>* outputs,
                               uint32_t* next_store,
                               std::vector<Step>* steps) {
  return Rewriter(source_columns, outputs, next_store, steps).Run();
}

}  // namespace perfetto::trace_processor::pipeline
