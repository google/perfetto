#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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
Tag vocabulary and table importance for stdlib modules.

Module tags themselves live in each module's SQL as a `-- @tags` directive; this
file only defines the valid tag vocabulary (VALID_TAGS) used to validate them, and
the per-table importance levels (TABLE_IMPORTANCE). Tags use a nested structure with
":" separators, e.g. "power:battery" enables both "power" and "power:battery".
"""

from typing import Optional

# Valid tags that can be used for categorizing modules.
# Tags should be short, descriptive, and help users find modules for their problems.
#
# Tag categories:
# - Platform/OS: android, chrome, linux
# - Hardware/Resources: cpu, gpu, memory, power
# - Performance: performance, startup
# - UI/Graphics: ui, input
# - App/Process: app-lifecycle
# - System: scheduling, ipc, virtualization
# - Analysis/Utilities: trace, utilities, metadata
VALID_TAGS = frozenset({
    # Platform/OS tags
    'android',
    'chrome',
    'linux',

    # Hardware/Resource tags
    'cpu',
    'gpu',
    'memory',
    'power',

    # Performance tags
    'performance',
    'startup',

    # UI/Graphics tags
    'ui',
    'input',

    # App/Process tags
    'app-lifecycle',

    # System tags
    'scheduling',
    'ipc',
    'virtualization',

    # Analysis/Utility tags
    'trace',
    'utilities',
    'metadata',
})

# Table importance levels for documentation.
# Importance levels help users discover the most relevant tables for their use case.
# Levels:
#   'core': Core tables - Fundamental built-in tables present in every trace
#   'high': Very frequent - Most commonly used, fundamental tables for trace analysis
#   'mid': Frequent - Important for specific use cases, moderately common
#   'low': Specialized or advanced tables, less frequently needed
#   None/absent: Normal importance (default)
TABLE_IMPORTANCE = {
    # CORE - Fundamental built-in tables present in every trace
    'slice': 'core',  # All slices (spans of time with a name)
    'counter': 'core',  # Time-series metrics: memory, battery, custom counters
    'thread': 'core',  # Thread metadata and information
    'process': 'core',  # Process metadata and information
    'track': 'core',  # Tracks for organizing slices and counters
    'sched': 'core',  # Kernel scheduling events table
    'thread_state':
        'core',  # CPU scheduling: what threads ran when and for how long

    # HIGH IMPORTANCE - Commonly used derived tables
    'thread_or_process_slice':
        'high',  # Unified slice table for thread and process slices

    # HIGH IMPORTANCE - Android UI performance analysis
    'android_frames': 'high',  # Frame rendering timeline for jank analysis
    'android_jank_cujs':
        'high',  # Completed user journeys with jank classifications

    # HIGH IMPORTANCE - Android app startup performance
    'android_startups': 'high',  # App launch events with timing breakdowns

    # HIGH IMPORTANCE - Android inter-process communication
    'android_binder_txns': 'high',  # Binder transactions for IPC analysis

    # HIGH IMPORTANCE - Android input and memory
    'android_input_events': 'high',  # Input event tracking and analysis
    'android_process_memory_intervals':
        'high',  # Per-process memory usage over time

    # HIGH IMPORTANCE - Profiling and virtualization
    'cpu_profile_stack_sample':
        'high',  # CPU profiling stack samples for performance analysis
    'pkvm_hypervisor_events':
        'high',  # pKVM hypervisor events for virtualization analysis

    # MID IMPORTANCE - Android system monitoring and diagnostics
    'android_anrs': 'mid',  # Application Not Responding events and diagnostics
    'android_battery_charge': 'mid',  # Battery charge level tracking over time
    'android_charging_states': 'mid',  # Device charging state transitions
    'android_process_metadata': 'mid',  # Process metadata and information
    'android_statsd_atoms': 'mid',  # StatsD atom events and counters

    # HIGH IMPORTANCE - Chrome performance analysis
    'chrome_event_latencies':
        'high',  # Input event latency tracking for jank analysis
    'chrome_tasks': 'high',  # Chrome task execution tracking

    # MID IMPORTANCE - Chrome graphics and metadata
    'chrome_graphics_pipeline_surface_frame_steps':
        'mid',  # Graphics pipeline pre-surface aggregation
    'chrome_graphics_pipeline_display_frame_steps':
        'mid',  # Graphics pipeline post-surface aggregation

    # LOW IMPORTANCE - Deprecated table names, kept for backward compatibility
    'slices': 'low',  # Raw slice table, prefer thread_or_process_slice
    'sched_slice': 'low',  # Raw scheduling slice table, prefer thread_state
    'counters': 'low',  # Raw counter table, prefer counter table
    'raw': 'low',
    'gpu_track': 'low',
}


def get_table_importance(table_name: str) -> Optional[str]:
  """Get the importance level of a table.

  Args:
    table_name: Name of the table/view

  Returns:
    The importance level ('core', 'high', 'mid', 'low') or None for normal importance
  """
  return TABLE_IMPORTANCE.get(table_name)
