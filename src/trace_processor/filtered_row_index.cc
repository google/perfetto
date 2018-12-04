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

#include "src/trace_processor/filtered_row_index.h"

namespace perfetto {
namespace trace_processor {

FilteredRowIndex::FilteredRowIndex(uint32_t start_row, uint32_t end_row)
    : mode_(Mode::kAllRows), start_row_(start_row), end_row_(end_row) {}

}  // namespace trace_processor
}  // namespace perfetto
