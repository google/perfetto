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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_

#include <cstdint>
#include <string>

namespace perfetto::trace_processor::pipeline {

// Maximum number of output columns supported by the `__intrinsic_pipeline`
// SQLite table function.
inline constexpr uint32_t kMaxPipelineColumns = 256;

// Table function that runs a serialized PreparedStatement.
inline constexpr char kPipelineFunction[] = "__intrinsic_pipeline";

// Aggregate function that materializes a SQL subquery into a dataframe.
inline constexpr char kDataframeAggFunction[] = "__intrinsic_dataframe_agg";

// Function that packs hoisted dataframe inputs into a single argument.
inline constexpr char kDataframesFunction[] = "__intrinsic_dataframes";

std::string QuoteIdentifier(const std::string& name);
std::string QuoteString(const std::string& text);

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PIPELINE_SQL_H_
