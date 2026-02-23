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

#ifndef SRC_TOOLS_HTTP_ADDITIONAL_CORS_ORIGINS_HTTP_ADDITIONAL_CORS_ORIGINS_H_
#define SRC_TOOLS_HTTP_ADDITIONAL_CORS_ORIGINS_HTTP_ADDITIONAL_CORS_ORIGINS_H_

#include <string>
#include <vector>

namespace perfetto {

// Returns a vector of strings containing additional CORS origins specified by
// the GN arg perfetto_http_additional_cors_origins.
std::vector<std::string> GetHttpAdditionalCorsOrigins();

}  // namespace perfetto

#endif  // SRC_TOOLS_HTTP_ADDITIONAL_CORS_ORIGINS_HTTP_ADDITIONAL_CORS_ORIGINS_H_
