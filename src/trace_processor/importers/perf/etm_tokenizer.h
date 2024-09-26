/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PERF_ETM_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PERF_ETM_TOKENIZER_H_

#include <memory>

#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/perf/aux_data_tokenizer.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {

base::StatusOr<std::unique_ptr<AuxDataTokenizerFactory>>
CreateEtmTokenizerFactory(TraceBlobView info);

}  // namespace perf_importer
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PERF_ETM_TOKENIZER_H_
