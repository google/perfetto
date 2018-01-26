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

#include "src/ftrace_reader/event_info.h"

namespace perfetto {

// TODO(hjd): Auto-generate this file.
// TODO(b/70373826): Reduce runtime overhead with constexpr magic etc.
std::vector<Event> GetStaticEventInfo() {
  std::vector<Event> events;

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "print";
    event->group = "ftrace";
    event->proto_field_id = 3;
    event->fields.push_back(FieldFromNameIdType("buf", 2, kProtoString));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "sched_switch";
    event->group = "sched";
    event->proto_field_id = 4;
    event->fields.push_back(FieldFromNameIdType("prev_comm", 1, kProtoString));
    event->fields.push_back(FieldFromNameIdType("prev_pid", 2, kProtoInt32));
    event->fields.push_back(FieldFromNameIdType("prev_prio", 3, kProtoInt32));
    event->fields.push_back(FieldFromNameIdType("prev_state", 4, kProtoInt64));
    event->fields.push_back(FieldFromNameIdType("next_comm", 5, kProtoString));
    event->fields.push_back(FieldFromNameIdType("next_pid", 6, kProtoInt32));
    event->fields.push_back(FieldFromNameIdType("next_prio", 7, kProtoInt32));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "cpufreq_interactive_already";
    event->group = "cpufreq_interactive";
    event->proto_field_id = 5;
    event->fields.push_back(FieldFromNameIdType("cpu_id", 1, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("load", 2, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("curtarg", 3, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("curactual", 4, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("newtarg", 5, kProtoUint64));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->proto_field_id = 6;
    event->name = "cpufreq_interactive_boost";
    event->group = "cpufreq_interactive";
    event->fields.push_back(FieldFromNameIdType("s", 1, kProtoString));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "cpufreq_interactive_notyet";
    event->group = "cpufreq_interactive";
    event->proto_field_id = 7;
    event->fields.push_back(FieldFromNameIdType("cpu_id", 1, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("load", 2, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("curtarg", 3, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("curactual", 4, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("newtarg", 5, kProtoUint64));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "cpufreq_interactive_setspeed";
    event->group = "cpufreq_interactive";
    event->proto_field_id = 8;
    event->fields.push_back(FieldFromNameIdType("cpu_id", 1, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("targfreq", 2, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("actualfreq", 3, kProtoUint64));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "cpufreq_interactive_target";
    event->group = "cpufreq_interactive";
    event->proto_field_id = 9;
    event->fields.push_back(FieldFromNameIdType("cpu_id", 1, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("load", 2, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("curtarg", 3, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("curactual", 4, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("newtarg", 5, kProtoUint64));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->proto_field_id = 10;
    event->name = "cpufreq_interactive_unboost";
    event->group = "cpufreq_interactive";
    event->fields.push_back(FieldFromNameIdType("s", 1, kProtoString));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "cpu_frequency";
    event->group = "power";
    event->proto_field_id = 11;
    event->fields.push_back(FieldFromNameIdType("state", 1, kProtoUint32));
    event->fields.push_back(FieldFromNameIdType("cpu_id", 2, kProtoUint32));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "cpu_frequency_limits";
    event->group = "power";
    event->proto_field_id = 12;
    event->fields.push_back(FieldFromNameIdType("min_freq", 1, kProtoUint32));
    event->fields.push_back(FieldFromNameIdType("max_freq", 2, kProtoUint32));
    event->fields.push_back(FieldFromNameIdType("cpu_id", 3, kProtoUint32));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "cpu_idle";
    event->group = "power";
    event->proto_field_id = 13;
    event->fields.push_back(FieldFromNameIdType("state", 1, kProtoUint32));
    event->fields.push_back(FieldFromNameIdType("cpu_id", 2, kProtoUint32));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "clock_enable";
    event->group = "power";
    event->proto_field_id = 14;
    event->fields.push_back(FieldFromNameIdType("name", 1, kProtoString));
    event->fields.push_back(FieldFromNameIdType("state", 2, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("cpu_id", 3, kProtoUint64));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "clock_disable";
    event->group = "power";
    event->proto_field_id = 15;
    event->fields.push_back(FieldFromNameIdType("name", 1, kProtoString));
    event->fields.push_back(FieldFromNameIdType("state", 2, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("cpu_id", 3, kProtoUint64));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "clock_set_rate";
    event->group = "power";
    event->proto_field_id = 16;
    event->fields.push_back(FieldFromNameIdType("name", 1, kProtoString));
    event->fields.push_back(FieldFromNameIdType("state", 2, kProtoUint64));
    event->fields.push_back(FieldFromNameIdType("cpu_id", 3, kProtoUint64));
  }

  return events;
}

}  // namespace perfetto
