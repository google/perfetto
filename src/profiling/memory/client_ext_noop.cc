/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "perfetto/profiling/memory/client_ext.h"

#include <inttypes.h>

__attribute__((visibility("default"))) AHeapInfo* AHeapInfo_create(
    const char*) {
  return nullptr;
}

__attribute__((visibility("default"))) AHeapInfo* AHeapInfo_setCallback(
    AHeapInfo*,
    void (*)(bool enabled)) {
  return nullptr;
}

__attribute__((visibility("default"))) uint32_t AHeapProfile_registerHeap(
    AHeapInfo*) {
  return 0;
}

__attribute__((visibility("default"))) bool
AHeapProfile_reportAllocation(uint32_t, uint64_t, uint64_t) {
  return false;
}

__attribute__((visibility("default"))) void AHeapProfile_reportFree(uint32_t,
                                                                    uint64_t) {}

__attribute__((visibility("default"))) bool AHeapProfile_initSession(
    void* (*)(size_t),
    void (*)(void*)) {
  return false;
}
