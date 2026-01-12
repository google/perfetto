#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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
"""
Data availability checks for high and mid importance SQL modules.

These are modules that create tables marked as 'high' or 'mid' importance
in stdlib_tags.py TABLE_IMPORTANCE dict.

Auto-generated - do not edit manually.
"""


def check_to_query(check: str) -> str:
  return f"SELECT EXISTS({check} LIMIT 1) AS has_data"


# Module name -> SQL query that checks if data exists
# Query returns 1 if data present, 0 if not
MODULE_DATA_CHECK_SQL = {
    # HIGH IMPORTANCE TABLES
    'android.binder':
        'SELECT 1 FROM slice WHERE name GLOB \'binder *\'',
    'android.cujs.cujs_base':
        'SELECT 1 FROM slice WHERE name GLOB \'J<*>\'',
    'android.frames.timeline':
        'SELECT 1 FROM slice WHERE name GLOB \'Choreographer#doFrame*\' OR name GLOB \'DrawFrame*\'',
    'android.startup.startups':
        'SELECT 1 FROM slice WHERE name IN (\'bindApplication\', \'activityStart\', \'activityResume\')',

    # MID IMPORTANCE TABLES
    'android.anrs':
        'SELECT 1 FROM slice WHERE name GLOB \'*ApplicationNotResponding*\'',
    'android.battery':
        'SELECT 1 FROM slice WHERE name GLOB \'batt.*\'',
    'android.battery.charging_states':
        'SELECT 1 FROM slice WHERE name = \'BatteryStatus\'',
    'android.battery_stats':
        'SELECT 1 FROM counter_track WHERE name GLOB \'battery_stats.*\'',
    'android.power_rails':
        'SELECT 1 FROM track WHERE type = \'power_rails\'',
    'android.process_metadata':
        'SELECT 1 FROM process',
    'android.screenshots':
        'SELECT 1 FROM slice WHERE name = \'Screenshot\' AND category = \'android_screenshot\'',
    'android.statsd':
        'SELECT 1 FROM track WHERE name = \'Statsd Atoms\'',
    'android.wakeups':
        'SELECT 1 FROM track WHERE name = \'wakeup_reason\'',

    # HIGH IMPORTANCE TABLES - Chrome
    'chrome.event_latency':
        'SELECT 1 FROM slice WHERE name = \'EventLatency\'',
    'chrome.tasks':
        'SELECT 1 FROM slice WHERE category GLOB \'*toplevel*\' AND (name = \'ThreadControllerImpl::RunTask\' OR name = \'ThreadPool_RunTask\')',

    # MID IMPORTANCE TABLES - Chrome
    'chrome.graphics_pipeline':
        'SELECT 1 FROM slice WHERE name = \'Graphics.Pipeline\'',
    'chrome.metadata':
        'SELECT 1 FROM metadata WHERE name = \'cr-hardware-class\' OR name GLOB \'cr-*\'',

    # LOW IMPORTANCE TABLES
    'chrome.android_input':
        'SELECT 1 FROM slice WHERE name GLOB \'UnwantedInteractionBlocker::notifyMotion*\'',
    'chrome.startups':
        'SELECT 1 FROM thread_slice WHERE name = \'Startup.ActivityStart\'',

    # PIXEL TABLES
    'pixel.camera':
        'SELECT 1 FROM slice WHERE name GLOB \'cam*_*:* (frame *)\'',
    # INTRINSIC-BASED TABLES - Android
    'android.cpu.cpu_per_uid':
        'SELECT 1 FROM __intrinsic_android_cpu_per_uid_track',
    'android.input':
        'SELECT 1 FROM __intrinsic_android_key_events',
    'android.kernel_wakelocks':
        'SELECT 1 FROM track WHERE name = \'android_kernel_wakelock\'',
    'android.network_packets':
        'SELECT 1 FROM __intrinsic_android_network_packets',
    'android.user_list':
        'SELECT 1 FROM __intrinsic_android_user_list',
    'android.winscope.inputmethod':
        'SELECT 1 FROM __intrinsic_inputmethod_clients',
    'android.winscope.rect':
        'SELECT 1 FROM __intrinsic_winscope_rect',
    'android.winscope.surfaceflinger':
        'SELECT 1 FROM __intrinsic_surfaceflinger_transaction',
    'android.winscope.transitions':
        'SELECT 1 FROM __intrinsic_window_manager_shell_transition_participants',
    'android.winscope.viewcapture':
        'SELECT 1 FROM __intrinsic_viewcapture',
    'android.winscope.windowmanager':
        'SELECT 1 FROM __intrinsic_windowmanager',

    # INTRINSIC-BASED TABLES - Linux
    'linux.perf.spe':
        'SELECT 1 FROM __intrinsic_spe_record',
    # INTRINSIC-BASED TABLES - V8/JIT
    'stack_trace.jit':
        'SELECT 1 FROM __intrinsic_jit_code',
    'v8.jit':
        'SELECT 1 FROM __intrinsic_v8_isolate',

    # INTRINSIC-BASED TABLES - Visualization
    'viz.track_event_callstacks':
        'SELECT 1 FROM __intrinsic_track_event_callstacks',

    # INTRINSIC-BASED TABLES - Prelude (single intrinsic only)
    'prelude.after_eof.counters':
        'SELECT 1 FROM __intrinsic_track',
    'prelude.after_eof.events':
        'SELECT 1 FROM __intrinsic_ftrace_event',
    'prelude.after_eof.tracks':
        'SELECT 1 FROM __intrinsic_track',
}
