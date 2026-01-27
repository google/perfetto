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

#include "src/tools/http_additional_cors_origins/http_additional_cors_origins.h"

#include "perfetto/ext/base/string_utils.h"

namespace perfetto {

std::vector<std::string> GetHttpAdditionalCorsOrigins() {
  return base::SplitString(PERFETTO_HTTP_ADDITIONAL_CORS_ORIGINS, ",");
}

}  // namespace perfetto
