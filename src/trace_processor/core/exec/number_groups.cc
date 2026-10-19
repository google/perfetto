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

#include "src/trace_processor/core/exec/number_groups.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/group_table.h"
#include "src/trace_processor/core/exec/row_batch.h"

namespace perfetto::trace_processor::core::exec {

NumberGroups::NumberGroups(std::vector<uint32_t> key_columns)
    : key_columns_(std::move(key_columns)) {}

NumberGroups::~NumberGroups() = default;
NumberGroups::State::~State() = default;

void NumberGroups::State::Reset() {
  groups = 0;
  kinds.clear();
  status = base::OkStatus();
}

std::unique_ptr<OperatorState> NumberGroups::MakeState(Context& context) const {
  auto state = std::make_unique<State>(context);
  state->words.resize(key_columns_.size());
  state->last_words.resize(key_columns_.size());
  return state;
}

base::Status NumberGroups::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

bool NumberGroups::Process(RowBatch& batch, OperatorState& state) const {
  State& s = state.Cast<State>();
  uint32_t count = batch.size();
  if (count == 0) {
    return true;
  }
  s.nulls.resize(count);
  memset(s.nulls.data(), 0, count * sizeof(uint64_t));
  s.starts.resize(count);
  uint8_t* starts = s.starts.data();

  // A row starts a new group if any key's null bit differs from the previous
  // row. The very first row also starts a group. So does the first row of a
  // batch whose key kinds differ from the previous batch.
  bool first = s.kinds.empty();
  bool kinds_change = false;
  for (uint32_t k = 0; k < key_columns_.size(); ++k) {
    s.words[k].resize(count);
    std::optional<KeyKind> kind =
        LoadKeyWords(batch.column(key_columns_[k]), batch.selection(), count, k,
                     s.words[k].data(), s.nulls.data());
    if (!kind) {
      s.status = base::ErrStatus("NUMBER GROUPS: column %u cannot be a key",
                                 key_columns_[k]);
      return false;
    }
    if (first) {
      s.kinds.push_back(*kind);
    } else if (s.kinds[k] != *kind) {
      s.kinds[k] = *kind;
      kinds_change = true;
    }
  }
  const uint64_t* nulls = s.nulls.data();
  starts[0] = first || kinds_change || nulls[0] != s.last_nulls;
  for (uint32_t i = 1; i < count; ++i) {
    starts[i] = nulls[i] != nulls[i - 1];
  }
  // A row also starts a new group if any key value differs from the previous
  // row.
  for (uint32_t k = 0; k < key_columns_.size(); ++k) {
    const int64_t* words = s.words[k].data();
    starts[0] |= words[0] != s.last_words[k];
    for (uint32_t i = 1; i < count; ++i) {
      starts[i] |= words[i] != words[i - 1];
    }
    s.last_words[k] = words[count - 1];
  }
  s.last_nulls = nulls[count - 1];

  ColumnBuffer buffer = s.context->TakeBuffer();
  uint32_t* numbers = buffer.chunk().Values<uint32_t>();
  const Selection& selection = batch.selection();
  uint32_t groups = s.groups;
  for (uint32_t i = 0; i < count; ++i) {
    groups += starts[i];
    numbers[selection[i]] = groups - 1;
  }
  s.groups = groups;
  batch.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, numbers),
                  std::move(buffer));
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
