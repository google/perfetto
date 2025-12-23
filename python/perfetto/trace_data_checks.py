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

# Module name -> SQL query that checks if data exists
# Query returns 1 if data present, 0 if not
MODULE_DATA_CHECK_SQL = {
    # HIGH IMPORTANCE TABLES
    'android.binder':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name GLOB \'binder *\' LIMIT 1) AS has_data',
    'android.cujs.cujs_base':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name GLOB \'J<*>\' LIMIT 1) AS has_data',
    'android.frames.timeline':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name GLOB \'Choreographer#doFrame*\' OR name GLOB \'DrawFrame*\' LIMIT 1) AS has_data',
    'android.startup.startups':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name IN (\'bindApplication\', \'activityStart\', \'activityResume\') LIMIT 1) AS has_data',
    'slices.with_context':
        'SELECT EXISTS(SELECT 1 FROM slice JOIN thread_track ON slice.track_id = thread_track.id LIMIT 1) AS has_data',

    # MID IMPORTANCE TABLES
    'android.anrs':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name GLOB \'*ApplicationNotResponding*\' LIMIT 1) AS has_data',
    'android.battery':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name GLOB \'batt.*\' LIMIT 1) AS has_data',
    'android.battery.charging_states':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name = \'BatteryStatus\' LIMIT 1) AS has_data',
    'android.battery_stats':
        'SELECT EXISTS(SELECT 1 FROM counter_track WHERE name GLOB \'battery_stats.*\' LIMIT 1) AS has_data',
    'android.process_metadata':
        'SELECT EXISTS(SELECT 1 FROM process LIMIT 1) AS has_data',
    'android.screenshots':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name = \'Screenshot\' AND category = \'android_screenshot\' LIMIT 1) AS has_data',
    'android.statsd':
        'SELECT EXISTS(SELECT 1 FROM track WHERE name = \'Statsd Atoms\' LIMIT 1) AS has_data',
    'android.wakeups':
        'SELECT EXISTS(SELECT 1 FROM track WHERE name = \'wakeup_reason\' LIMIT 1) AS has_data',

    # HIGH IMPORTANCE TABLES - Chrome
    'chrome.event_latency':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name = \'EventLatency\' LIMIT 1) AS has_data',
    'chrome.tasks':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE category GLOB \'*toplevel*\' AND (name = \'ThreadControllerImpl::RunTask\' OR name = \'ThreadPool_RunTask\') LIMIT 1) AS has_data',

    # MID IMPORTANCE TABLES - Chrome
    'chrome.graphics_pipeline':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name = \'Graphics.Pipeline\' LIMIT 1) AS has_data',
    'chrome.metadata':
        'SELECT EXISTS(SELECT 1 FROM metadata WHERE name = \'cr-hardware-class\' OR name GLOB \'cr-*\' LIMIT 1) AS has_data',

    # LOW IMPORTANCE TABLES
    'chrome.android_input':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name GLOB \'UnwantedInteractionBlocker::notifyMotion*\' LIMIT 1) AS has_data',

    # PIXEL TABLES
    'pixel.camera':
        'SELECT EXISTS(SELECT 1 FROM slice WHERE name GLOB \'cam*_*:* (frame *)\' LIMIT 1) AS has_data',

    # INTRINSIC-BASED TABLES - Android
    'android.cpu.cpu_per_uid':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_android_cpu_per_uid_track LIMIT 1) AS has_data',
    'android.input':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_android_key_events LIMIT 1) AS has_data',
    'android.network_packets':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_android_network_packets LIMIT 1) AS has_data',
    'android.user_list':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_android_user_list LIMIT 1) AS has_data',
    'android.winscope.inputmethod':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_inputmethod_clients LIMIT 1) AS has_data',
    'android.winscope.rect':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_winscope_rect LIMIT 1) AS has_data',
    'android.winscope.surfaceflinger':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_surfaceflinger_transaction LIMIT 1) AS has_data',
    'android.winscope.transitions':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_window_manager_shell_transition_participants LIMIT 1) AS has_data',
    'android.winscope.viewcapture':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_viewcapture LIMIT 1) AS has_data',
    'android.winscope.windowmanager':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_windowmanager LIMIT 1) AS has_data',

    # INTRINSIC-BASED TABLES - Linux
    'linux.perf.spe':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_spe_record LIMIT 1) AS has_data',

    # INTRINSIC-BASED TABLES - V8/JIT
    'stack_trace.jit':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_jit_code LIMIT 1) AS has_data',
    'v8.jit':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_v8_isolate LIMIT 1) AS has_data',

    # INTRINSIC-BASED TABLES - Visualization
    'viz.track_event_callstacks':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_track_event_callstacks LIMIT 1) AS has_data',

    # INTRINSIC-BASED TABLES - Prelude (single intrinsic only)
    'prelude.after_eof.counters':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_track LIMIT 1) AS has_data',
    'prelude.after_eof.events':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_ftrace_event LIMIT 1) AS has_data',
    'prelude.after_eof.tracks':
        'SELECT EXISTS(SELECT 1 FROM __intrinsic_track LIMIT 1) AS has_data',
}
