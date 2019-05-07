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

#ifndef SRC_TRACE_PROCESSOR_METRICS_METRICS_H_
#define SRC_TRACE_PROCESSOR_METRICS_METRICS_H_

#include <sqlite3.h>
#include <unordered_map>
#include <vector>

#include "perfetto/trace_processor/trace_processor.h"

namespace perfetto {
namespace trace_processor {
namespace metrics {

// Replaces templated variables inside |raw_text| using the substitution given
// by |substitutions| writing the result to |out|.
// The syntax followed is a cut-down variant of Jinja. This means variables that
// are to be replaced use {{variable-name}} in the raw text with subsitutions
// containing a mapping from (variable-name -> replacement).
int TemplateReplace(
    const std::string& raw_text,
    const std::unordered_map<std::string, std::string>& substitutions,
    std::string* out);

// This function implements the RUN_METRIC SQL function.
void RunMetric(sqlite3_context* ctx, int argc, sqlite3_value** argv);

int ComputeMetrics(TraceProcessor* impl,
                   const std::vector<std::string>& metric_names,
                   std::vector<uint8_t>* metrics_proto);

}  // namespace metrics
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_METRICS_METRICS_H_
