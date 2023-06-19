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

#include "perfetto/public/producer.h"
#include "perfetto/public/te_category_macros.h"
#include "perfetto/public/track_event.h"

#include <stdio.h>
#include <unistd.h>

#define EXAMPLE_CATEGORIES(C)                                   \
  C(rendering, "rendering", "Rendering events", "tag1", "tag2") \
  C(physics, "physics", "Physics events", "tag1")               \
  C(cat, "cat", "Sample category")                              \
  C(c3, "c3", "c3", "tag1", "tag2", "tag3")                     \
  C(c4, "c4", "c4", "tag1", "tag2", "tag3", "tag4")

PERFETTO_TE_CATEGORIES_DEFINE(EXAMPLE_CATEGORIES)

static void EnabledCb(struct PerfettoTeCategoryImpl* c,
                      PerfettoDsInstanceIndex inst_id,
                      bool enabled,
                      bool global_state_changed,
                      void* user_arg) {
  printf("Callback: %p id: %u on: %d, global_state_changed: %d, user_arg:%p\n",
         (void*)c, inst_id, (int)enabled, (int)global_state_changed, user_arg);
}

int main(void) {
  struct PerfettoProducerInitArgs args = {0};
  args.backends = PERFETTO_BACKEND_SYSTEM;
  PerfettoProducerInit(args);
  PerfettoTeInit();
  PERFETTO_TE_REGISTER_CATEGORIES(EXAMPLE_CATEGORIES);
  PerfettoTeCategorySetCallback(&physics, EnabledCb, NULL);
  for (;;) {
    sleep(1);
  }
}
