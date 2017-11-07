/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <stdio.h>
#include <unistd.h>

#include "ftrace_reader/ftrace_controller.h"
#include "ftrace_reader/ftrace_cpu_reader.h"

int main(int argc, const char** argv) {
  perfetto::FtraceController ftrace_controller;

  ftrace_controller.ClearTrace();
  ftrace_controller.WriteTraceMarker("Hello, world!");

  for (int i = 1; i < argc; i++) {
    printf("Enabling: %s\n", argv[i]);
    ftrace_controller.EnableEvent(argv[i]);
  }

  // Sleep for one second so we get some events
  sleep(1);

  for (int i = 1; i < argc; i++) {
    printf("Disable: %s\n", argv[i]);
    ftrace_controller.DisableEvent(argv[i]);
  }

  return perfetto::SomePublicApi();
}
