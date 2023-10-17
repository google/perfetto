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
from diff_tests.chrome.tests import Chrome
from diff_tests.chrome.tests_args import ChromeArgs
from diff_tests.chrome.tests_memory_snapshots import ChromeMemorySnapshots
from diff_tests.chrome.tests_processes import ChromeProcesses
from diff_tests.chrome.tests_rail_modes import ChromeRailModes
from diff_tests.chrome.tests_scroll_jank import ChromeScrollJank
from diff_tests.chrome.tests_touch_gesture import ChromeTouchGesture
from diff_tests.graphics.tests import Graphics
from diff_tests.graphics.tests_drm_related_ftrace_events import GraphicsDrmRelatedFtraceEvents
from diff_tests.graphics.tests_gpu_trace import GraphicsGpuTrace
from diff_tests.metrics.camera.tests import Camera
from diff_tests.metrics.codecs.tests import Codecs
from diff_tests.metrics.frame_timeline.tests import FrameTimeline
from diff_tests.metrics.irq.tests import IRQ
from diff_tests.metrics.memory.tests import MemoryMetrics
from diff_tests.metrics.network.tests import NetworkMetrics
from diff_tests.metrics.profiling.tests import ProfilingMetrics
from diff_tests.metrics.startup.tests import Startup
from diff_tests.metrics.startup.tests_broadcasts import StartupBroadcasts
from diff_tests.metrics.startup.tests_lock_contention import StartupLockContention
from diff_tests.metrics.startup.tests_metrics import StartupMetrics
from diff_tests.metrics.webview.tests import WebView
from diff_tests.parser.android_fs.tests import AndroidFs
from diff_tests.parser.atrace.tests import Atrace
from diff_tests.parser.atrace.tests_error_handling import AtraceErrorHandling
from diff_tests.parser.cros.tests import Cros
from diff_tests.parser.fs.tests import Fs
from diff_tests.parser.fuchsia.tests import Fuchsia
from diff_tests.parser.memory.tests import MemoryParser
from diff_tests.parser.network.tests import NetworkParser
from diff_tests.parser.parsing.tests import Parsing
from diff_tests.parser.parsing.tests_debug_annotation import ParsingDebugAnnotation
from diff_tests.parser.parsing.tests_memory_counters import ParsingMemoryCounters
from diff_tests.parser.parsing.tests_rss_stats import ParsingRssStats
from diff_tests.parser.process_tracking.tests import ProcessTracking
from diff_tests.parser.profiling.tests import Profiling
from diff_tests.parser.profiling.tests_heap_graph import ProfilingHeapGraph
from diff_tests.parser.profiling.tests_heap_profiling import ProfilingHeapProfiling
from diff_tests.parser.profiling.tests_llvm_symbolizer import ProfilingLlvmSymbolizer
from diff_tests.parser.sched.tests import SchedParser
from diff_tests.parser.smoke.tests import Smoke
from diff_tests.parser.smoke.tests_compute_metrics import SmokeComputeMetrics
from diff_tests.parser.smoke.tests_json import SmokeJson
from diff_tests.parser.smoke.tests_sched_events import SmokeSchedEvents
from diff_tests.parser.track_event.tests import TrackEvent
from diff_tests.parser.translated_args.tests import TranslatedArgs
from diff_tests.parser.ufs.tests import Ufs
from diff_tests.power.tests import Power
from diff_tests.power.tests_energy_breakdown import PowerEnergyBreakdown
from diff_tests.power.tests_entity_state_residency import EntityStateResidency
from diff_tests.power.tests_linux_sysfs_power import LinuxSysfsPower
from diff_tests.power.tests_power_rails import PowerPowerRails
from diff_tests.power.tests_voltage_and_scaling import PowerVoltageAndScaling
from diff_tests.stdlib.dynamic_tables.tests import DynamicTables
from diff_tests.stdlib.pkvm.tests import Pkvm
from diff_tests.stdlib.slices.tests import Slices
from diff_tests.stdlib.span_join.tests_left_join import SpanJoinLeftJoin
from diff_tests.stdlib.span_join.tests_outer_join import SpanJoinOuterJoin
from diff_tests.stdlib.span_join.tests_regression import SpanJoinRegression
from diff_tests.stdlib.span_join.tests_smoke import SpanJoinSmoke
from diff_tests.stdlib.timestamps.tests import Timestamps
from diff_tests.syntax.functions.tests import Functions
from diff_tests.syntax.perfetto_sql.tests import PerfettoSql
from diff_tests.tables.tests import Tables
from diff_tests.tables.tests_counters import TablesCounters
from diff_tests.tables.tests_sched import TablesSched

sys.path.pop()


def fetch_all_diff_tests(index_path: str) -> List['testing.TestCase']:
  parser_tests = [
      *AndroidFs(index_path, 'parser/android_fs', 'AndroidFs').fetch(),
      *Atrace(index_path, 'parser/atrace', 'Atrace').fetch(),
      *AtraceErrorHandling(index_path, 'parser/atrace',
                           'AtraceErrorHandling').fetch(),
      *Cros(index_path, 'parser/cros', 'Cros').fetch(),
      *Fs(index_path, 'parser/fs', 'Fs').fetch(),
      *Fuchsia(index_path, 'parser/fuchsia', 'Fuchsia').fetch(),
      *MemoryParser(index_path, 'parser/memory', 'MemoryParser').fetch(),
      *NetworkParser(index_path, 'parser/network', 'NetworkParser').fetch(),
      *ProcessTracking(index_path, 'parser/process_tracking',
                       'ProcessTracking').fetch(),
      *Profiling(index_path, 'parser/profiling', 'Profiling').fetch(),
      *ProfilingHeapProfiling(index_path, 'parser/profiling',
                              'ProfilingHeapProfiling').fetch(),
      *ProfilingHeapGraph(index_path, 'parser/profiling',
                          'ProfilingHeapGraph').fetch(),
      *ProfilingLlvmSymbolizer(index_path, 'parser/profiling',
                               'ProfilingLlvmSymbolizer').fetch(),
      *Smoke(index_path, 'parser/smoke', 'Smoke').fetch(),
      *SchedParser(index_path, 'parser/sched', 'SchedParser').fetch(),
      *SmokeComputeMetrics(index_path, 'parser/smoke',
                           'SmokeComputeMetrics').fetch(),
      *SmokeJson(index_path, 'parser/smoke', 'SmokeJson').fetch(),
      *SmokeSchedEvents(index_path, 'parser/smoke', 'SmokeSchedEvents').fetch(),
      *TrackEvent(index_path, 'parser/track_event', 'TrackEvent').fetch(),
      *TranslatedArgs(index_path, 'parser/translated_args',
                      'TranslatedArgs').fetch(),
      *Ufs(index_path, 'parser/ufs', 'Ufs').fetch(),
      # TODO(altimin, lalitm): "parsing" should be split into more specific directories.
      *Parsing(index_path, 'parser/parsing', 'Parsing').fetch(),
      *ParsingDebugAnnotation(index_path, 'parser/parsing',
                              'ParsingDebugAnnotation').fetch(),
      *ParsingRssStats(index_path, 'parser/parsing', 'ParsingRssStats').fetch(),
      *ParsingMemoryCounters(index_path, 'parser/parsing',
                             'ParsingMemoryCounters').fetch(),
  ]

  metrics_tests = [
      *Camera(index_path, 'metrics/camera', 'Camera').fetch(),
      *Codecs(index_path, 'metrics/codecs', 'Codecs').fetch(),
      *MemoryMetrics(index_path, 'metrics/memory', 'MemoryMetrics').fetch(),
      *NetworkMetrics(index_path, 'metrics/network', 'NetworkMetrics').fetch(),
      *FrameTimeline(index_path, 'metrics/frame_timeline',
                     'FrameTimeline').fetch(),
      *IRQ(index_path, 'metrics/irq', 'IRQ').fetch(),
      *ProfilingMetrics(index_path, 'metrics/profiling',
                        'ProfilingMetrics').fetch(),
      *Startup(index_path, 'metrics/startup', 'Startup').fetch(),
      *StartupBroadcasts(index_path, 'metrics/startup',
                         'StartupBroadcasts').fetch(),
      *StartupMetrics(index_path, 'metrics/startup', 'StartupMetrics').fetch(),
      *StartupLockContention(index_path, 'metrics/startup',
                             'StartupLockContention').fetch(),
      *WebView(index_path, 'metrics/webview', 'WebView').fetch(),
  ]

  stdlib_tests = [
      *DynamicTables(index_path, 'stdlib/dynamic_tables',
                     'DynamicTables').fetch(),
      *Pkvm(index_path, 'stdlib/pkvm', 'Pkvm').fetch(),
      *Slices(index_path, 'stdlib/slices', 'Slices').fetch(),
      *SpanJoinLeftJoin(index_path, 'stdlib/span_join',
                        'SpanJoinLeftJoin').fetch(),
      *SpanJoinOuterJoin(index_path, 'stdlib/span_join',
                         'SpanJoinOuterJoin').fetch(),
      *SpanJoinSmoke(index_path, 'stdlib/span_join', 'SpanJoinSmoke').fetch(),
      *SpanJoinRegression(index_path, 'stdlib/span_join',
                          'SpanJoinRegression').fetch(),
      *Timestamps(index_path, 'stdlib/timestamps', 'Timestamps').fetch(),
  ]

  syntax_tests = [
      *Functions(index_path, 'syntax/functions', 'Functions').fetch(),
      *PerfettoSql(index_path, 'syntax/perfetto_sql', 'PerfettoSql').fetch(),
  ]

  return parser_tests + metrics_tests + stdlib_tests + syntax_tests + [
      *Android(index_path, 'android', 'Android').fetch(),
      *AndroidBugreport(index_path, 'android', 'AndroidBugreport').fetch(),
      *AndroidGames(index_path, 'android', 'AndroidGames').fetch(),
      *ChromeScrollJank(index_path, 'chrome', 'ChromeScrollJank').fetch(),
      *ChromeTouchGesture(index_path, 'chrome', 'ChromeTouchGesture').fetch(),
      *ChromeMemorySnapshots(index_path, 'chrome',
                             'ChromeMemorySnapshots').fetch(),
      *ChromeRailModes(index_path, 'chrome', 'ChromeRailModes').fetch(),
      *ChromeProcesses(index_path, 'chrome', 'ChromeProcesses').fetch(),
      *ChromeArgs(index_path, 'chrome', 'ChromeArgs').fetch(),
      *Chrome(index_path, 'chrome', 'Chrome').fetch(),
      *EntityStateResidency(index_path, 'power',
                            'EntityStateResidency').fetch(),
      *Graphics(index_path, 'graphics', 'Graphics').fetch(),
      *GraphicsGpuTrace(index_path, 'graphics', 'GraphicsGpuTrace').fetch(),
      *GraphicsDrmRelatedFtraceEvents(index_path, 'graphics',
                                      'GraphicsDrmRelatedFtraceEvents').fetch(),
      *LinuxSysfsPower(index_path, 'power', 'LinuxSysfsPower').fetch(),
      *Power(index_path, 'power', 'Power').fetch(),
      *PowerPowerRails(index_path, 'power', 'PowerPowerRails').fetch(),
      *PowerVoltageAndScaling(index_path, 'power',
                              'PowerVoltageAndScaling').fetch(),
      *PowerEnergyBreakdown(index_path, 'power',
                            'PowerEnergyBreakdown').fetch(),
      *SurfaceFlingerLayers(index_path, 'android',
                            'SurfaceFlingerLayers').fetch(),
      *SurfaceFlingerTransactions(index_path, 'android',
                                  'SurfaceFlingerTransactions').fetch(),
      *Tables(index_path, 'tables', 'Tables').fetch(),
      *TablesCounters(index_path, 'tables', 'TablesCounters').fetch(),
      *TablesSched(index_path, 'tables', 'TablesSched').fetch(),
  ]
