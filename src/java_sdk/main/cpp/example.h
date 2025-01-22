/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_JAVA_SDK_MAIN_CPP_EXAMPLE_H_
#define SRC_JAVA_SDK_MAIN_CPP_EXAMPLE_H_

#include <string>

#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/tracing.h"
#include "perfetto/tracing/track_event.h"

// The set of track event categories that the example is using.
PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("rendering")
        .SetDescription("Rendering and graphics events"),
    perfetto::Category("network.debug")
        .SetTags("debug")
        .SetDescription("Verbose network events"),
    perfetto::Category("audio.latency")
        .SetTags("verbose")
        .SetDescription("Detailed audio latency metrics"));

int run_main(const std::string output_file_path);

#endif  // SRC_JAVA_SDK_MAIN_CPP_EXAMPLE_H_
