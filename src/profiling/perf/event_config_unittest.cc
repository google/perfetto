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

#include "src/profiling/perf/event_config.h"

#include <linux/perf_event.h>
#include <stdint.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/config/data_source_config.pbzero.h"
#include "protos/perfetto/config/profiling/perf_event_config.pbzero.h"

namespace perfetto {
namespace profiling {
namespace {

static DataSourceConfig ConfigForTid(int32_t tid) {
  protozero::HeapBuffered<protos::pbzero::PerfEventConfig> pb_config;
  pb_config->set_tid(tid);
  protozero::HeapBuffered<protos::pbzero::DataSourceConfig> ds_config;
  ds_config->set_perf_event_config_raw(pb_config.SerializeAsString());
  DataSourceConfig cfg;
  PERFETTO_CHECK(cfg.ParseFromString(ds_config.SerializeAsString()));
  return cfg;
}

TEST(EventConfigTest, TidRequired) {
  // Doesn't pass validation without a TID
  DataSourceConfig cfg;
  ASSERT_TRUE(cfg.ParseFromString(""));

  base::Optional<EventConfig> event_config = EventConfig::Create(cfg);
  ASSERT_FALSE(event_config.has_value());
}

TEST(EventConfigTest, AttrStructConstructed) {
  auto cfg = ConfigForTid(42);
  base::Optional<EventConfig> event_config = EventConfig::Create(cfg);

  ASSERT_TRUE(event_config.has_value());
  ASSERT_TRUE(event_config->perf_attr() != nullptr);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
