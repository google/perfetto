/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PUBLIC_TRACK_EVENT_H_
#define INCLUDE_PERFETTO_PUBLIC_TRACK_EVENT_H_

#include <stdint.h>
#include <stdlib.h>

#include "perfetto/public/abi/track_event_abi.h"  // IWYU pragma: export
#include "perfetto/public/compiler.h"
#include "perfetto/public/data_source.h"
#include "perfetto/public/pb_msg.h"

// A registered category.
struct PerfettoTeCategory {
  PERFETTO_ATOMIC(bool) * enabled;
  struct PerfettoTeCategoryImpl* impl;
  struct PerfettoTeCategoryDescriptor desc;
  uint64_t cat_iid;
};

// Registers the category `cat`. `cat->desc` must be filled before calling this.
// The rest of the structure is filled by the function.
static inline void PerfettoTeCategoryRegister(struct PerfettoTeCategory* cat) {
  cat->impl = PerfettoTeCategoryImplCreate(&cat->desc);
  cat->enabled = PerfettoTeCategoryImplGetEnabled(cat->impl);
  cat->cat_iid = PerfettoTeCategoryImplGetIid(cat->impl);
}

// Calls PerfettoTeCategoryRegister() on multiple categories.
static inline void PerfettoTeRegisterCategories(
    struct PerfettoTeCategory* cats[],
    size_t size) {
  for (size_t i = 0; i < size; i++) {
    PerfettoTeCategoryRegister(cats[i]);
  }
}

// Registers `cb` to be called every time a data source instance with `reg_cat`
// enabled is created or destroyed. `user_arg` will be passed unaltered to `cb`.
//
// `cb` can be NULL to disable the callback.
static inline void PerfettoTeCategorySetCallback(
    struct PerfettoTeCategory* reg_cat,
    PerfettoTeCategoryImplCallback cb,
    void* user_arg) {
  PerfettoTeCategoryImplSetCallback(reg_cat->impl, cb, user_arg);
}

// Unregisters the category `cat`.
//
// WARNING: The category cannot be used for tracing anymore after this.
// Executing PERFETTO_TE() on an unregistered category will cause a null pointer
// dereference.
static inline void PerfettoTeCategoryUnregister(
    struct PerfettoTeCategory* cat) {
  PerfettoTeCategoryImplDestroy(cat->impl);
  cat->impl = PERFETTO_NULL;
  cat->enabled = &perfetto_atomic_false;
  cat->cat_iid = 0;
}

// Calls PerfettoTeCategoryUnregister() on multiple categories.
//
// WARNING: The categories cannot be used for tracing anymore after this.
// Executing PERFETTO_TE() on unregistered categories will cause a null pointer
// dereference.
static inline void PerfettoTeUnregisterCategories(
    struct PerfettoTeCategory* cats[],
    size_t size) {
  for (size_t i = 0; i < size; i++) {
    PerfettoTeCategoryUnregister(cats[i]);
  }
}

#endif  // INCLUDE_PERFETTO_PUBLIC_TRACK_EVENT_H_
