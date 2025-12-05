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
    'android.suspend':
        'SELECT EXISTS(SELECT 1 FROM track WHERE name IN (\'Suspend/Resume Minimal\', \'Suspend/Resume Latency\') LIMIT 1) AS has_data',
    'android.statsd':
        'SELECT EXISTS(SELECT 1 FROM track WHERE name = \'Statsd Atoms\' LIMIT 1) AS has_data',
    'android.wakeups':
        'SELECT EXISTS(SELECT 1 FROM track WHERE name = \'wakeup_reason\' LIMIT 1) AS has_data',
}
