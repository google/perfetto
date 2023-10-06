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
import os
import sys
from typing import List

from python.generators.diff_tests import testing

# A hack to import using `diff_tests.` which prevents the risk name conflicts,
# i.e importing a module when user has a different package of the same name
# installed.
TRACE_PROCESSOR_TEST_DIR = os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))
sys.path.append(TRACE_PROCESSOR_TEST_DIR)

from diff_tests.android.tests import Android
from diff_tests.android.tests_bugreport import AndroidBugreport
from diff_tests.android.tests_games import AndroidGames
from diff_tests.android.tests_surfaceflinger_layers import SurfaceFlingerLayers
from diff_tests.android.tests_surfaceflinger_transactions import SurfaceFlingerTransactions
from diff_tests.atrace.tests import Atrace
from diff_tests.atrace.tests_error_handling import AtraceErrorHandling
from diff_tests.camera.tests import Camera
from diff_tests.chrome.tests import Chrome
from diff_tests.chrome.tests_args import ChromeArgs
from diff_tests.chrome.tests_memory_snapshots import ChromeMemorySnapshots
from diff_tests.chrome.tests_processes import ChromeProcesses
from diff_tests.chrome.tests_rail_modes import ChromeRailModes
from diff_tests.chrome.tests_scroll_jank import ChromeScrollJank
from diff_tests.chrome.tests_touch_gesture import ChromeTouchGesture
from diff_tests.codecs.tests import Codecs
from diff_tests.cros.tests import Cros
from diff_tests.dynamic.tests import Dynamic
from diff_tests.fs.tests import Fs
from diff_tests.fuchsia.tests import Fuchsia
from diff_tests.functions.tests import Functions
from diff_tests.graphics.tests import Graphics
from diff_tests.graphics.tests_drm_related_ftrace_events import \
    GraphicsDrmRelatedFtraceEvents
from diff_tests.graphics.tests_gpu_trace import GraphicsGpuTrace
from diff_tests.memory.tests import Memory
from diff_tests.memory.tests_metrics import MemoryMetrics
from diff_tests.network.tests import Network
from diff_tests.parsing.tests import Parsing
from diff_tests.parsing.tests_debug_annotation import ParsingDebugAnnotation
from diff_tests.parsing.tests_memory_counters import ParsingMemoryCounters
from diff_tests.parsing.tests_rss_stats import ParsingRssStats
from diff_tests.perfetto_sql.tests import PerfettoSql
from diff_tests.performance.tests import Performance
from diff_tests.pkvm.tests import Pkvm
from diff_tests.power.tests import Power
from diff_tests.power.tests_energy_breakdown import PowerEnergyBreakdown
from diff_tests.power.tests_entity_state_residency import EntityStateResidency
from diff_tests.power.tests_linux_sysfs_power import LinuxSysfsPower
from diff_tests.power.tests_power_rails import PowerPowerRails
from diff_tests.power.tests_voltage_and_scaling import PowerVoltageAndScaling
from diff_tests.process_tracking.tests import ProcessTracking
from diff_tests.profiling.tests import Profiling
from diff_tests.profiling.tests_heap_graph import ProfilingHeapGraph
from diff_tests.profiling.tests_heap_profiling import ProfilingHeapProfiling
from diff_tests.profiling.tests_llvm_symbolizer import ProfilingLlvmSymbolizer
from diff_tests.profiling.tests_metrics import ProfilingMetrics
from diff_tests.scheduler.tests import Scheduler
from diff_tests.slices.tests import Slices
from diff_tests.smoke.tests import Smoke
from diff_tests.smoke.tests_compute_metrics import SmokeComputeMetrics
from diff_tests.smoke.tests_json import SmokeJson
from diff_tests.smoke.tests_sched_events import SmokeSchedEvents
from diff_tests.span_join.tests_left_join import SpanJoinLeftJoin
from diff_tests.span_join.tests_outer_join import SpanJoinOuterJoin
from diff_tests.span_join.tests_regression import SpanJoinRegression
from diff_tests.span_join.tests_smoke import SpanJoinSmoke
from diff_tests.startup.tests import Startup
from diff_tests.startup.tests_broadcasts import StartupBroadcasts
from diff_tests.startup.tests_lock_contention import StartupLockContention
from diff_tests.startup.tests_metrics import StartupMetrics
from diff_tests.tables.tests import Tables
from diff_tests.tables.tests_counters import TablesCounters
from diff_tests.tables.tests_sched import TablesSched
from diff_tests.time.tests import Time
from diff_tests.track_event.tests import TrackEvent
from diff_tests.translation.tests import Translation
from diff_tests.ufs.tests import Ufs
from diff_tests.webview.tests import WebView
from diff_tests.android_fs.tests import AndroidFs

sys.path.pop()


def fetch_all_diff_tests(index_path: str) -> List['testing.TestCase']:
  return [
      *Android(index_path, 'android', 'Android').fetch(),
      *AndroidBugreport(index_path, 'android', 'AndroidBugreport').fetch(),
      *AndroidFs(index_path, 'android_fs', 'AndroidFs').fetch(),
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
      *Codecs(index_path, 'codecs', 'Codecs').fetch(),
      *Cros(index_path, 'cros', 'Cros').fetch(),
      *Dynamic(index_path, 'dynamic', 'Dynamic').fetch(),
      *EntityStateResidency(index_path, 'power',
                            'EntityStateResidency').fetch(),
      *Fs(index_path, 'fs', 'Fs').fetch(),
      *Fuchsia(index_path, 'fuchsia', 'Fuchsia').fetch(),
      *Functions(index_path, 'functions', 'Functions').fetch(),
      *Graphics(index_path, 'graphics', 'Graphics').fetch(),
      *GraphicsGpuTrace(index_path, 'graphics', 'GraphicsGpuTrace').fetch(),
      *GraphicsDrmRelatedFtraceEvents(index_path, 'graphics',
                                      'GraphicsDrmRelatedFtraceEvents').fetch(),
      *Ufs(index_path, 'ufs', 'Ufs').fetch(),
      *LinuxSysfsPower(index_path, 'power', 'LinuxSysfsPower').fetch(),
      *Memory(index_path, 'memory', 'Memory').fetch(),
      *MemoryMetrics(index_path, 'memory', 'MemoryMetrics').fetch(),
      *Network(index_path, 'network', 'Network').fetch(),
      *Parsing(index_path, 'parsing', 'Parsing').fetch(),
      *ParsingDebugAnnotation(index_path, 'parsing',
                              'ParsingDebugAnnotation').fetch(),
      *ParsingRssStats(index_path, 'parsing', 'ParsingRssStats').fetch(),
      *ParsingMemoryCounters(index_path, 'parsing',
                             'ParsingMemoryCounters').fetch(),
      *PerfettoSql(index_path, 'perfetto_sql', 'PerfettoSql').fetch(),
      *Performance(index_path, 'performance', 'Performance').fetch(),
      *Pkvm(index_path, 'pkvm', 'Pkvm').fetch(),
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
      *Slices(index_path, 'slices', 'Slices').fetch(),
      *Smoke(index_path, 'smoke', 'Smoke').fetch(),
      *SmokeComputeMetrics(index_path, 'smoke', 'SmokeComputeMetrics').fetch(),
      *SmokeJson(index_path, 'smoke', 'SmokeJson').fetch(),
      *SmokeSchedEvents(index_path, 'smoke', 'SmokeSchedEvents').fetch(),
      *SpanJoinLeftJoin(index_path, 'span_join', 'SpanJoinLeftJoin').fetch(),
      *SpanJoinOuterJoin(index_path, 'span_join', 'SpanJoinOuterJoin').fetch(),
      *SpanJoinSmoke(index_path, 'span_join', 'SpanJoinSmoke').fetch(),
      *SpanJoinRegression(index_path, 'span_join',
                          'SpanJoinRegression').fetch(),
      *Startup(index_path, 'startup', 'Startup').fetch(),
      *StartupBroadcasts(index_path, 'startup', 'StartupBroadcasts').fetch(),
      *StartupMetrics(index_path, 'startup', 'StartupMetrics').fetch(),
      *StartupLockContention(index_path, 'startup',
                             'StartupLockContention').fetch(),
      *SurfaceFlingerLayers(index_path, 'android',
                            'SurfaceFlingerLayers').fetch(),
      *SurfaceFlingerTransactions(index_path, 'android',
                                  'SurfaceFlingerTransactions').fetch(),
      *Tables(index_path, 'tables', 'Tables').fetch(),
      *TablesCounters(index_path, 'tables', 'TablesCounters').fetch(),
      *TablesSched(index_path, 'tables', 'TablesSched').fetch(),
      *Time(index_path, 'time', 'Time').fetch(),
      *TrackEvent(index_path, 'track_event', 'TrackEvent').fetch(),
      *Translation(index_path, 'translation', 'Translation').fetch(),
      *WebView(index_path, 'webview', 'WebView').fetch(),
  ]
