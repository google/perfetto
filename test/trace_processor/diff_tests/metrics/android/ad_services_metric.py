#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from os import sys, path

import synth_common

UI_NOTIFICATION_TRIGGER_EVENT = "NotificationTriggerEvent"
AD_ID_CACHE_EVENT = "AdIdCacheEvent"
APP_SET_ID_EVENT = "AppSetIdEvent"

trace = synth_common.create_trace()

trace.add_ftrace_packet(cpu=0)

trace.add_sys_enter(ts=100, tid=42, id=64)
trace.add_sys_exit(ts=200, tid=42, id=64, ret=0)

trace.add_atrace_begin(
    ts=350, tid=42, pid=42, buf=UI_NOTIFICATION_TRIGGER_EVENT)
trace.add_atrace_end(ts=650, tid=42, pid=42)

trace.add_atrace_begin(ts=750, tid=42, pid=42, buf=AD_ID_CACHE_EVENT)
trace.add_atrace_end(ts=850, tid=42, pid=42)

trace.add_atrace_begin(ts=900, tid=42, pid=42, buf=APP_SET_ID_EVENT)
trace.add_atrace_end(ts=1200, tid=42, pid=42)

sys.stdout.buffer.write(trace.trace.SerializeToString())
