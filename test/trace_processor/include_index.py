#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
from typing import List
from python.generators.diff_tests import testing


from android.tests import Android
from android.tests_bugreport import AndroidBugreport
from android.tests_games import AndroidGames
from atrace.tests import Atrace
from atrace.tests_error_handling import AtraceErrorHandling
from camera.tests import Camera
from chrome.tests_scroll_jank import ChromeScrollJank
from chrome.tests_touch_gesture import ChromeTouchGesture
from chrome.tests_memory_snapshots import ChromeMemorySnapshots
from chrome.tests_rail_modes import ChromeRailModes
from chrome.tests_processes import ChromeProcesses
from chrome.tests_args import ChromeArgs
from chrome.tests import Chrome
from cros.tests import Cros
from dynamic.tests import Dynamic
from fs.tests import Fs
from fuchsia.tests import Fuchsia
from functions.tests import Functions
from graphics.tests import Graphics
from graphics.tests_gpu_trace import GraphicsGpuTrace
from graphics.tests_drm_related_ftrace_events import GraphicsDrmRelatedFtraceEvents
from ufs.tests import Ufs
from memory.tests import Memory
from memory.tests_metrics import MemoryMetrics
from network.tests import Network
from parsing.tests import Parsing
from parsing.tests_rss_stats import ParsingRssStats
from parsing.tests_memory_counters import ParsingMemoryCounters
from performance.tests import Performance
from power.tests import Power
from power.tests_power_rails import PowerPowerRails
from power.tests_voltage_and_scaling import PowerVoltageAndScaling
from power.tests_energy_breakdown import PowerEnergyBreakdown
from process_tracking.tests import ProcessTracking
from profiling.tests import Profiling
from profiling.tests_heap_profiling import ProfilingHeapProfiling
from profiling.tests_heap_graph import ProfilingHeapGraph
from profiling.tests_metrics import ProfilingMetrics
from profiling.tests_llvm_symbolizer import ProfilingLlvmSymbolizer
from scheduler.tests import Scheduler
from smoke.tests import Smoke
from smoke.tests_json import SmokeJson
from smoke.tests_sched_events import SmokeSchedEvents
from smoke.tests_compute_metrics import SmokeComputeMetrics
from span_join.tests_outer_join import SpanJoinOuterJoin
from span_join.tests_left_join import SpanJoinLeftJoin
from span_join.tests_smoke import SpanJoinSmoke
from span_join.tests_regression import SpanJoinRegression
from startup.tests import Startup
from startup.tests_broadcasts import StartupBroadcasts
from startup.tests_metrics import StartupMetrics
from startup.tests_lock_contention import StartupLockContention
from tables.tests import Tables
from tables.tests_counters import TablesCounters
from tables.tests_sched import TablesSched
from track_event.tests import TrackEvent
from translation.tests import Translation


def fetch_all_diff_tests(index_path: str) -> List['testing.TestCase']:
  return [
      *Android(index_path, 'android', 'Android').fetch(),
      *AndroidBugreport(index_path, 'android', 'AndroidBugreport').fetch(),
      *AndroidGames(index_path, 'android', 'AndroidGames').fetch(),
      *Atrace(index_path, 'atrace', 'Atrace').fetch(),
      *AtraceErrorHandling(index_path, 'atrace', 'AtraceErrorHandling').fetch(),
      *Camera(index_path, 'camera', 'Camera').fetch(),
      *ChromeScrollJank(index_path, 'chrome', 'ChromeScrollJank').fetch(),
      *ChromeTouchGesture(index_path, 'chrome', 'ChromeTouchGesture').fetch(),
      *ChromeMemorySnapshots(index_path, 'chrome',
                             'ChromeMemorySnapshots').fetch(),
      *ChromeRailModes(index_path, 'chrome', 'ChromeRailModes').fetch(),
      *ChromeProcesses(index_path, 'chrome', 'ChromeProcesses').fetch(),
      *ChromeArgs(index_path, 'chrome', 'ChromeArgs').fetch(),
      *Chrome(index_path, 'chrome', 'Chrome').fetch(),
      *Cros(index_path, 'cros', 'Cros').fetch(),
      *Dynamic(index_path, 'dynamic', 'Dynamic').fetch(),
      *Fs(index_path, 'fs', 'Fs').fetch(),
      *Fuchsia(index_path, 'fuchsia', 'Fuchsia').fetch(),
      *Functions(index_path, 'functions', 'Functions').fetch(),
      *Graphics(index_path, 'graphics', 'Graphics').fetch(),
      *GraphicsGpuTrace(index_path, 'graphics', 'GraphicsGpuTrace').fetch(),
      *GraphicsDrmRelatedFtraceEvents(index_path, 'graphics',
                                      'GraphicsDrmRelatedFtraceEvents').fetch(),
      *Ufs(index_path, 'ufs', 'Ufs').fetch(),
      *Memory(index_path, 'memory', 'Memory').fetch(),
      *MemoryMetrics(index_path, 'memory', 'MemoryMetrics').fetch(),
      *Network(index_path, 'network', 'Network').fetch(),
      *Parsing(index_path, 'parsing', 'Parsing').fetch(),
      *ParsingRssStats(index_path, 'parsing', 'ParsingRssStats').fetch(),
      *ParsingMemoryCounters(index_path, 'parsing',
                             'ParsingMemoryCounters').fetch(),
      *Performance(index_path, 'performance', 'Performance').fetch(),
      *Power(index_path, 'power', 'Power').fetch(),
      *PowerPowerRails(index_path, 'power', 'PowerPowerRails').fetch(),
      *PowerVoltageAndScaling(index_path, 'power',
                              'PowerVoltageAndScaling').fetch(),
      *PowerEnergyBreakdown(index_path, 'power',
                            'PowerEnergyBreakdown').fetch(),
      *ProcessTracking(index_path, 'process_tracking',
                       'ProcessTracking').fetch(),
      *Profiling(index_path, 'profiling', 'Profiling').fetch(),
      *ProfilingHeapProfiling(index_path, 'profiling',
                              'ProfilingHeapProfiling').fetch(),
      *ProfilingHeapGraph(index_path, 'profiling',
                          'ProfilingHeapGraph').fetch(),
      *ProfilingMetrics(index_path, 'profiling', 'ProfilingMetrics').fetch(),
      *ProfilingLlvmSymbolizer(index_path, 'profiling',
                               'ProfilingLlvmSymbolizer').fetch(),
      *Scheduler(index_path, 'scheduler', 'Scheduler').fetch(),
      *Smoke(index_path, 'smoke', 'Smoke').fetch(),
      *SmokeJson(index_path, 'smoke', 'SmokeJson').fetch(),
      *SmokeSchedEvents(index_path, 'smoke', 'SmokeSchedEvents').fetch(),
      *SmokeComputeMetrics(index_path, 'smoke', 'SmokeComputeMetrics').fetch(),
      *SpanJoinOuterJoin(index_path, 'span_join', 'SpanJoinOuterJoin').fetch(),
      *SpanJoinLeftJoin(index_path, 'span_join', 'SpanJoinLeftJoin').fetch(),
      *SpanJoinSmoke(index_path, 'span_join', 'SpanJoinSmoke').fetch(),
      *SpanJoinRegression(index_path, 'span_join',
                          'SpanJoinRegression').fetch(),
      *Startup(index_path, 'startup', 'Startup').fetch(),
      *StartupBroadcasts(index_path, 'startup', 'StartupBroadcasts').fetch(),
      *StartupMetrics(index_path, 'startup', 'StartupMetrics').fetch(),
      *StartupLockContention(index_path, 'startup',
                             'StartupLockContention').fetch(),
      *Tables(index_path, 'tables', 'Tables').fetch(),
      *TablesCounters(index_path, 'tables', 'TablesCounters').fetch(),
      *TablesSched(index_path, 'tables', 'TablesSched').fetch(),
      *TrackEvent(index_path, 'track_event', 'TrackEvent').fetch(),
      *Translation(index_path, 'translation', 'Translation').fetch(),
  ]
