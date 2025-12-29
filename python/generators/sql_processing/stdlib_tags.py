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
Tags for stdlib modules.

This module provides a mapping from module names to tags for categorizing
and searching stdlib modules. Tags use a nested structure with ":" separators,
e.g., "power:battery" means both "power" and "power:battery" tags are enabled.
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

# Dictionary mapping module names to their tags
# All tags must be from the VALID_TAGS set above
MODULE_TAGS = {
    # Android - Power & Battery
    'android.battery': ['android', 'power'],
    'android.battery.charging_states': ['android', 'power'],
    'android.battery.doze': ['android', 'power'],
    'android.battery_stats': ['android', 'power'],
    'android.power_rails': ['android', 'power'],
    'android.wakeups': ['android', 'power'],
    'android.suspend': ['android', 'power'],

    # Android - CPU
    'android.cpu.cluster_type': ['android', 'cpu'],
    'android.cpu.cpu_per_uid': ['android', 'cpu'],
    'android.dvfs': ['android', 'cpu', 'power'],

    # Android - GPU
    'android.gpu.frequency': ['android', 'gpu'],
    'android.gpu.mali_power_state': ['android', 'gpu', 'power'],
    'android.gpu.work_period': ['android', 'gpu'],
    'android.gpu.memory': ['android', 'gpu', 'memory'],

    # Android - Memory
    'android.memory.heap_graph.dominator_tree': ['android', 'memory'],
    'android.memory.heap_graph.helpers': ['android', 'memory'],
    'android.memory.heap_graph.excluded_refs': ['android', 'memory'],
    'android.memory.heap_graph.dominator_class_tree': ['android', 'memory'],
    'android.memory.heap_graph.raw_dominator_tree': ['android', 'memory'],
    'android.memory.heap_graph.class_relationship': ['android', 'memory'],
    'android.memory.heap_graph.class_summary_tree': ['android', 'memory'],
    'android.memory.heap_graph.heap_graph_class_aggregation': [
        'android', 'memory'
    ],
    'android.memory.heap_profile.callstacks': ['android', 'memory'],
    'android.memory.heap_profile.summary_tree': ['android', 'memory'],
    'android.memory.dmabuf': ['android', 'memory'],
    'android.memory.lmk': ['android', 'memory'],
    'android.memory.process': ['android', 'memory'],
    'android.dumpsys.show_map': ['android', 'memory'],

    # Android - UI & Graphics
    'android.frames.per_frame_metrics': ['android', 'ui'],
    'android.frames.timeline': ['android', 'ui'],
    'android.frames.timeline_maxsdk28': ['android', 'ui'],
    'android.frames.jank_type': ['android', 'ui'],
    'android.frame_blocking_calls.blocking_calls_aggregation': [
        'android', 'ui'
    ],
    'android.cujs.base': ['android', 'ui'],
    'android.cujs.sysui_cujs': ['android', 'ui'],
    'android.cujs.threads': ['android', 'ui'],
    'android.input': ['android', 'ui', 'input'],
    'android.screenshots': ['android', 'ui'],
    'android.surfaceflinger': ['android', 'ui'],
    'android.winscope.surfaceflinger': ['android', 'ui'],
    'android.winscope.transitions': ['android', 'ui'],
    'android.winscope.rect': ['android', 'ui'],
    'android.winscope.inputmethod': ['android', 'ui', 'input'],
    'android.winscope.viewcapture': ['android', 'ui'],
    'android.winscope.windowmanager': ['android', 'ui'],
    'android.desktop_mode': ['android', 'ui'],
    'android.bitmaps': ['android', 'ui', 'memory'],
    'android.screen_state': ['android', 'ui'],

    # Android - App Lifecycle & Startup
    'android.startup.startups': ['android', 'startup'],
    'android.startup.startups_minsdk29': ['android', 'startup'],
    'android.startup.startups_maxsdk28': ['android', 'startup'],
    'android.startup.startup_events': ['android', 'startup'],
    'android.startup.startup_breakdowns': ['android', 'startup'],
    'android.startup.time_to_display': ['android', 'startup'],
    'android.app_process_starts': ['android', 'app-lifecycle'],
    'android.freezer': ['android', 'app-lifecycle'],
    'android.broadcasts': ['android', 'app-lifecycle'],
    'android.services': ['android', 'app-lifecycle'],
    'android.job_scheduler': ['android', 'app-lifecycle'],
    'android.job_scheduler_states': ['android', 'app-lifecycle'],
    'android.anrs': ['android', 'app-lifecycle', 'performance'],

    # Android - IPC & Communication
    'android.binder': ['android', 'ipc'],
    'android.binder_breakdown': ['android', 'ipc'],
    'android.network_packets': ['android'],

    # Android - System
    'android.version': ['android', 'metadata'],
    'android.slices': ['android', 'trace'],
    'android.user_list': ['android', 'metadata'],
    'android.auto.multiuser': ['android', 'metadata'],
    'android.entity_state_residency': ['android', 'power'],
    'android.device': ['android', 'metadata'],
    'android.process_metadata': ['android', 'metadata'],
    'android.thread': ['android', 'metadata'],
    'android.monitor_contention': ['android', 'performance'],
    'android.oom_adjuster': ['android', 'app-lifecycle', 'memory'],
    'android.kernel_wakelocks': ['android', 'power'],
    'android.statsd': ['android', 'metadata'],
    'android.garbage_collection': ['android', 'memory', 'performance'],

    # Chrome - Performance & Benchmarks
    'chrome.speedometer': ['chrome', 'performance'],
    'chrome.speedometer_2_1': ['chrome', 'performance'],
    'chrome.speedometer_3': ['chrome', 'performance'],

    # Chrome - Input & Interaction
    'chrome.event_latency': ['chrome', 'input'],
    'chrome.event_latency_description': ['chrome', 'input'],
    'chrome.input': ['chrome', 'input'],
    'chrome.android_input': ['chrome', 'input'],
    'chrome.scroll_interactions': ['chrome', 'input'],
    'chrome.interactions': ['chrome', 'input'],
    'chrome.web_content_interactions': ['chrome', 'input'],

    # Chrome - UI & Jank
    'chrome.graphics_pipeline': ['chrome', 'ui'],
    'chrome.vsync_intervals': ['chrome', 'ui'],
    'chrome.scroll_jank.scroll_jank_cause_map': ['chrome', 'ui'],
    'chrome.scroll_jank.predictor_error': ['chrome', 'ui'],
    'chrome.scroll_jank.scroll_offsets': ['chrome', 'ui'],
    'chrome.scroll_jank.scroll_jank_v3_cause': ['chrome', 'ui'],
    'chrome.scroll_jank.scroll_jank_cause_utils': ['chrome', 'ui'],
    'chrome.scroll_jank.scroll_jank_intervals': ['chrome', 'ui'],
    'chrome.scroll_jank.scroll_jank_v3': ['chrome', 'ui'],
    'chrome.scroll_jank.utils': ['chrome', 'ui'],
    'chrome.scroll_jank_tagging': ['chrome', 'ui'],
    'chrome.chrome_scrolls': ['chrome', 'ui'],

    # Chrome - Other
    'chrome.page_loads': ['chrome'],
    'chrome.startups': ['chrome', 'startup'],
    'chrome.tasks': ['chrome'],
    'chrome.histograms': ['chrome'],
    'chrome.metadata': ['chrome'],

    # Linux - CPU
    'linux.cpu.frequency': ['linux', 'cpu'],
    'linux.cpu.idle': ['linux', 'cpu', 'power'],
    'linux.cpu.idle_time_in_state': ['linux', 'cpu', 'power'],
    'linux.cpu.idle_stats': ['linux', 'cpu', 'power'],
    'linux.cpu.utilization.general': ['linux', 'cpu'],
    'linux.cpu.utilization.process': ['linux', 'cpu'],
    'linux.cpu.utilization.slice': ['linux', 'cpu'],
    'linux.cpu.utilization.system': ['linux', 'cpu'],
    'linux.cpu.utilization.thread': ['linux', 'cpu'],
    'linux.cpu.utilization.thread_cpu': ['linux', 'cpu'],

    # Linux - Other
    'linux.memory.general': ['linux', 'memory'],
    'linux.memory.high_watermark': ['linux', 'memory'],
    'linux.memory.process': ['linux', 'memory'],
    'linux.block_io': ['linux'],
    'linux.irqs': ['linux'],
    'linux.devfreq': ['linux', 'power'],
    'linux.perf.samples': ['linux', 'performance'],
    'linux.perf.spe': ['linux', 'performance'],
    'linux.threads': ['linux'],

    # Scheduling
    'sched.states': ['scheduling', 'cpu'],
    'sched.runnable': ['scheduling', 'cpu'],
    'sched.thread_level_parallelism': ['scheduling', 'cpu'],
    'sched.latency': ['scheduling', 'cpu', 'performance'],
    'sched.time_in_state': ['scheduling', 'cpu'],
    'sched.with_context': ['scheduling'],

    # Slices & Traces
    'slices.hierarchy': ['trace'],
    'slices.flat_slices': ['trace'],
    'slices.flow': ['trace'],
    'slices.with_context': ['trace'],
    'slices.cpu_time': ['trace', 'cpu'],
    'slices.self_dur': ['trace'],
    'slices.stack': ['trace'],
    'slices.time_in_state': ['trace'],

    # Graphs & Analysis
    'graphs.hierarchy': ['utilities'],
    'graphs.partition': ['utilities'],
    'graphs.dominator_tree': ['utilities'],
    'graphs.search': ['utilities'],
    'graphs.critical_path': ['utilities', 'performance'],

    # Utilities
    'time.conversion': ['utilities'],
    'proto_path.proto_path': ['utilities'],
    'counters.global_tracks': ['utilities'],
    'counters.intervals': ['utilities'],

    # Visualization
    'viz.threads': ['utilities'],
    'viz.summary.threads': ['utilities'],
    'viz.summary.counters': ['utilities'],
    'viz.summary.slices': ['utilities'],
    'viz.summary.processes': ['utilities'],
    'viz.summary.trace': ['utilities'],

    # Traced
    'traced.stats': ['metadata'],

    # Intervals
    'intervals.intersect': ['utilities'],
    'intervals.overlap': ['utilities'],

    # Export
    'export.to_firefox_profile': ['utilities'],

    # Stacks
    'stacks.cpu_profiling': ['performance', 'cpu'],

    # v8
    'v8.jit': ['performance'],

    # Pixel
    'pixel.camera': ['android'],

    # AppleOS
    'appleos.instruments.samples': ['performance'],

    # Wattson (Power modeling)
    'wattson.curves.gpu': ['power', 'gpu'],
    'wattson.cpu.hotplug': ['power', 'cpu'],

    # pKVM
    'pkvm.hypervisor': ['virtualization'],

    # Prelude - Core foundational modules automatically imported
    'prelude.after_eof.casts': ['utilities'],
    'prelude.after_eof.core': ['metadata'],
    'prelude.after_eof.counters': ['utilities'],
    'prelude.after_eof.cpu_scheduling': ['cpu', 'scheduling'],
    'prelude.after_eof.events': ['trace'],
    'prelude.after_eof.memory': ['memory'],
    'prelude.after_eof.slices': ['trace'],
    'prelude.after_eof.tracks': ['trace'],
    'prelude.after_eof.views': ['trace'],
    'prelude.before_eof.trace_bounds': ['metadata'],
}


def get_tags(module_name: str):
  """Get tags for a module name.

  Args:
    module_name: Module name (e.g., "android.battery")

  Returns:
    List of tags for the module, or empty list if no tags defined
  """
  return MODULE_TAGS.get(module_name, [])


def get_all_unique_tags():
  """Get all unique tags across all modules.

  Returns:
    Sorted list of all unique tags
  """
  all_tags = set()
  for tags in MODULE_TAGS.values():
    all_tags.update(tags)
  return sorted(all_tags)


# Validate that all tags in MODULE_TAGS are from VALID_TAGS
def _validate_tags():
  """Validate that all tags in MODULE_TAGS are valid and properly structured.

  Checks:
    1. All tags are from VALID_TAGS
    2. Nested tags (with ':') have their parent tags present

  Raises:
    ValueError: If any invalid tags are found or nested tags lack parents
  """
  invalid_tags_by_module = {}
  missing_parent_tags_by_module = {}

  for module_name, tags in MODULE_TAGS.items():
    # Check 1: All tags must be from VALID_TAGS
    invalid = [tag for tag in tags if tag not in VALID_TAGS]
    if invalid:
      invalid_tags_by_module[module_name] = invalid

    # Check 2: Nested tags must have their parent tags present
    tags_set = set(tags)
    missing_parents = []
    for tag in tags:
      if ':' in tag:
        parent = tag.split(':')[0]
        if parent not in tags_set:
          missing_parents.append(f"{tag} (missing parent: {parent})")
    if missing_parents:
      missing_parent_tags_by_module[module_name] = missing_parents

  # Report all errors
  errors = []

  if invalid_tags_by_module:
    error_lines = []
    for module_name in sorted(invalid_tags_by_module.keys()):
      error_lines.append(
          f"  {module_name}: {invalid_tags_by_module[module_name]}")
    errors.append(
        f"Invalid tags found in MODULE_TAGS (must be from VALID_TAGS):\n" +
        "\n".join(error_lines))

  if missing_parent_tags_by_module:
    error_lines = []
    for module_name in sorted(missing_parent_tags_by_module.keys()):
      error_lines.append(
          f"  {module_name}: {missing_parent_tags_by_module[module_name]}")
    errors.append(f"Nested tags must include their parent tags:\n" +
                  "\n".join(error_lines))

  if errors:
    raise ValueError("\n\n".join(errors))


# Run validation on module import
_validate_tags()

# Table importance levels for documentation.
# Importance levels help users discover the most relevant tables for their use case.
# Levels:
#   'high': Very frequent - Most commonly used, fundamental tables for trace analysis
#   'mid': Frequent - Important for specific use cases, moderately common
#   'low': Specialized or advanced tables, less frequently needed
#   None/absent: Normal importance (default)
TABLE_IMPORTANCE = {
    # HIGH IMPORTANCE - Core timestamped tables, fundamental for most analyses
    'thread_state':
        'high',  # CPU scheduling: what threads ran when and for how long
    'sched': 'high',  # Kernel scheduling events table
    'thread': 'high',  # Thread metadata and information
    'process': 'high',  # Process metadata and information
    'counter': 'high',  # Time-series metrics: memory, battery, custom counters
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

    # LOW IMPORTANCE - Raw/specialized tables, less frequently needed
    'slices':
        'low',  # Raw slice table, prefer thread_or_process_slice for most use cases
    'sched_slice':
        'low',  # Raw scheduling slice table, prefer thread_state for most use cases
}


def get_table_importance(table_name: str) -> Optional[str]:
  """Get the importance level of a table.

  Args:
    table_name: Name of the table/view

  Returns:
    The importance level ('high', 'mid', 'low') or None for normal importance
  """
  return TABLE_IMPORTANCE.get(table_name)
