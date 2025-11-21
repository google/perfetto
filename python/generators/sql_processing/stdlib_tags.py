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

# Dictionary mapping module names to their tags
# Tags should be short, descriptive, and help users find modules for their problems
# Use nested tags (with :) for important subcategories that users would search for
MODULE_TAGS = {
    # Android - Power & Battery
    'android.battery': ['power', 'power:battery'],
    'android.battery.charging_states': ['power', 'power:battery'],
    'android.battery.doze': ['power', 'power:battery'],
    'android.battery_stats': ['power', 'power:battery'],
    'android.power_rails': ['power', 'power:rails'],
    'android.wakeups': ['power', 'power:wakeup'],
    'android.suspend': ['power', 'power:wakeup'],

    # Android - CPU
    'android.cpu.cluster_type': ['cpu'],
    'android.cpu.cpu_per_uid': ['cpu', 'per-app'],
    'android.dvfs': ['cpu', 'power'],

    # Android - GPU
    'android.gpu.frequency': ['gpu'],
    'android.gpu.mali_power_state': ['gpu', 'power'],
    'android.gpu.work_period': ['gpu'],

    # Android - Memory
    'android.memory.heap_graph.dominator_tree': ['memory', 'memory:heap'],
    'android.memory.heap_graph.helpers': ['memory', 'memory:heap'],
    'android.memory.heap_graph.excluded_refs': ['memory', 'memory:heap'],
    'android.memory.heap_graph.dominator_class_tree': ['memory', 'memory:heap'],
    'android.memory.heap_graph.raw_dominator_tree': ['memory', 'memory:heap'],
    'android.memory.heap_profile.callstacks': ['memory', 'memory:heap'],
    'android.dumpsys.show_map': ['memory'],

    # Android - UI & Graphics
    'android.frames.per_frame_metrics': ['ui', 'ui:frames'],
    'android.frames.timeline': ['ui', 'ui:frames'],
    'android.frames.timeline_maxsdk28': ['ui', 'ui:frames'],
    'android.frames.jank_type': ['ui', 'ui:frames', 'ui:jank'],
    'android.frame_blocking_calls.blocking_calls_aggregation': [
        'ui', 'ui:frames'
    ],
    'android.cujs.cujs_base': ['ui', 'ui:jank'],
    'android.cujs.sysui_cujs': ['ui', 'ui:jank'],
    'android.input': ['ui', 'input'],
    'android.screenshots': ['ui'],
    'android.surfaceflinger': ['ui'],
    'android.winscope.surfaceflinger': ['ui'],
    'android.winscope.transitions': ['ui'],
    'android.winscope.rect': ['ui'],
    'android.winscope.inputmethod': ['ui', 'input'],
    'android.desktop_mode': ['ui'],

    # Android - App Lifecycle & Startup
    'android.startup.startups': ['startup'],
    'android.startup.startups_minsdk29': ['startup'],
    'android.startup.startups_maxsdk28': ['startup'],
    'android.startup.startup_events': ['startup'],
    'android.startup.startup_breakdowns': ['startup'],
    'android.startup.time_to_display': ['startup'],
    'android.app_process_starts': ['app-lifecycle'],
    'android.freezer': ['app-lifecycle'],
    'android.broadcasts': ['app-lifecycle'],
    'android.services': ['app-lifecycle'],
    'android.job_scheduler': ['app-lifecycle'],

    # Android - IPC & Communication
    'android.binder': ['ipc', 'ipc:binder'],
    'android.binder_breakdown': ['ipc', 'ipc:binder'],
    'android.network_packets': ['network'],

    # Android - System
    'android.version': [],
    'android.slices': [],
    'android.user_list': [],
    'android.auto.multiuser': [],
    'android.entity_state_residency': ['power'],

    # Chrome - Performance & Benchmarks
    'chrome.speedometer': ['chrome', 'chrome:benchmarks'],
    'chrome.speedometer_2_1': ['chrome', 'chrome:benchmarks'],
    'chrome.speedometer_3': ['chrome', 'chrome:benchmarks'],

    # Chrome - Input & Interaction
    'chrome.event_latency': ['chrome', 'input'],
    'chrome.event_latency_description': ['chrome', 'input'],
    'chrome.input': ['chrome', 'input'],
    'chrome.android_input': ['chrome', 'input'],
    'chrome.scroll_interactions': ['chrome', 'input', 'chrome:scroll'],
    'chrome.interactions': ['chrome', 'input'],
    'chrome.web_content_interactions': ['chrome', 'input'],

    # Chrome - UI & Jank
    'chrome.graphics_pipeline': ['chrome', 'ui'],
    'chrome.vsync_intervals': ['chrome', 'ui'],
    'chrome.scroll_jank.scroll_jank_cause_map': [
        'chrome', 'ui:jank', 'chrome:scroll'
    ],
    'chrome.scroll_jank.predictor_error': [
        'chrome', 'ui:jank', 'chrome:scroll'
    ],
    'chrome.scroll_jank.scroll_offsets': ['chrome', 'chrome:scroll'],
    'chrome.scroll_jank.scroll_jank_v3_cause': [
        'chrome', 'ui:jank', 'chrome:scroll'
    ],

    # Chrome - Other
    'chrome.page_loads': ['chrome', 'chrome:navigation'],
    'chrome.startups': ['chrome', 'startup'],
    'chrome.tasks': ['chrome'],
    'chrome.histograms': ['chrome'],
    'chrome.metadata': ['chrome'],

    # Linux - CPU
    'linux.cpu.frequency': ['linux', 'cpu', 'cpu:frequency'],
    'linux.cpu.idle': ['linux', 'cpu', 'cpu:idle', 'power'],
    'linux.cpu.idle_time_in_state': ['linux', 'cpu', 'cpu:idle', 'power'],
    'linux.cpu.idle_stats': ['linux', 'cpu', 'cpu:idle', 'power'],
    'linux.cpu.utilization.general': ['linux', 'cpu'],

    # Linux - Other
    'linux.memory.general': ['linux', 'memory'],
    'linux.block_io': ['linux', 'io'],
    'linux.irqs': ['linux'],

    # Scheduling
    'sched.states': ['scheduling', 'cpu'],
    'sched.runnable': ['scheduling', 'cpu'],
    'sched.thread_level_parallelism': ['scheduling', 'cpu'],

    # Slices & Traces
    'slices.hierarchy': ['trace'],
    'slices.flat_slices': ['trace'],
    'slices.flow': ['trace'],

    # Graphs & Analysis
    'graphs.hierarchy': ['analysis'],
    'graphs.partition': ['analysis'],
    'graphs.dominator_tree': ['analysis'],
    'graphs.search': ['analysis'],
    'graphs.critical_path': ['analysis', 'performance'],

    # Utilities
    'time.conversion': [],
    'proto_path.proto_path': [],
    'counters.global_tracks': [],

    # Visualization
    'viz.threads': [],
    'viz.summary.threads': [],
    'viz.summary.counters': [],
    'viz.summary.slices': [],
    'viz.summary.processes': [],
    'viz.summary.trace': [],

    # Traced
    'traced.stats': [],

    # Wattson (Power modeling)
    'wattson.curves.device_gpu': ['power', 'gpu'],
    'wattson.cpu.hotplug': ['power', 'cpu'],

    # pKVM
    'pkvm.hypervisor': [],
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
