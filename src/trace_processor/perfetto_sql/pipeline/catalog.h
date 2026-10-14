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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_CATALOG_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_CATALOG_H_

#include <string_view>

#include "src/perfetto_sql/analysis/relation.h"
#include "src/trace_processor/core/dataframe/dataframe.h"

namespace perfetto::trace_processor::pipeline {

// Lookup interface the compiler uses to resolve what a pipeline reads: the
// relations semantic analysis can describe, and the dataframes a pipeline can
// read directly.
class Catalog : public perfetto_sql::analysis::Catalog {
 public:
  ~Catalog() override;

  // Dataframe registered as `name`, or null.
  virtual const dataframe::Dataframe* FindDataframe(
      std::string_view name) const = 0;
};

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_CATALOG_H_
