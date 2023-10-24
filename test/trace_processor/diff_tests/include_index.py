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

from diff_tests.metrics.android.tests import AndroidMetrics
from diff_tests.metrics.camera.tests import Camera
from diff_tests.metrics.chrome.tests import ChromeMetrics
from diff_tests.metrics.chrome.tests_args import ChromeArgs
from diff_tests.metrics.chrome.tests_processes import ChromeProcesses
from diff_tests.metrics.chrome.tests_rail_modes import ChromeRailModes
from diff_tests.metrics.chrome.tests_scroll_jank import ChromeScrollJankMetrics
from diff_tests.metrics.chrome.tests_touch_gesture import ChromeTouchGesture
from diff_tests.metrics.codecs.tests import Codecs
from diff_tests.metrics.frame_timeline.tests import FrameTimeline
from diff_tests.metrics.graphics.tests import GraphicsMetrics
from diff_tests.metrics.irq.tests import IRQ
from diff_tests.metrics.memory.tests import MemoryMetrics
from diff_tests.metrics.network.tests import NetworkMetrics
from diff_tests.metrics.power.tests import Power
from diff_tests.metrics.profiling.tests import ProfilingMetrics
from diff_tests.metrics.startup.tests import Startup
from diff_tests.metrics.startup.tests_broadcasts import StartupBroadcasts
from diff_tests.metrics.startup.tests_lock_contention import StartupLockContention
from diff_tests.metrics.startup.tests_metrics import StartupMetrics
from diff_tests.metrics.webview.tests import WebView
from diff_tests.parser.android.tests import AndroidParser
from diff_tests.parser.android.tests_bugreport import AndroidBugreport
from diff_tests.parser.android.tests_games import AndroidGames
from diff_tests.parser.android.tests_surfaceflinger_layers import SurfaceFlingerLayers
from diff_tests.parser.android.tests_surfaceflinger_transactions import SurfaceFlingerTransactions
from diff_tests.parser.android_fs.tests import AndroidFs
from diff_tests.parser.atrace.tests import Atrace
from diff_tests.parser.atrace.tests_error_handling import AtraceErrorHandling
from diff_tests.parser.chrome.tests import ChromeParser
from diff_tests.parser.chrome.tests_memory_snapshots import ChromeMemorySnapshots
from diff_tests.parser.cros.tests import Cros
from diff_tests.parser.fs.tests import Fs
from diff_tests.parser.fuchsia.tests import Fuchsia
from diff_tests.parser.graphics.tests import GraphicsParser
from diff_tests.parser.graphics.tests_drm_related_ftrace_events import GraphicsDrmRelatedFtraceEvents
from diff_tests.parser.graphics.tests_gpu_trace import GraphicsGpuTrace
from diff_tests.parser.memory.tests import MemoryParser
from diff_tests.parser.network.tests import NetworkParser
from diff_tests.parser.parsing.tests import Parsing
from diff_tests.parser.parsing.tests_debug_annotation import ParsingDebugAnnotation
from diff_tests.parser.parsing.tests_memory_counters import ParsingMemoryCounters
from diff_tests.parser.parsing.tests_rss_stats import ParsingRssStats
from diff_tests.parser.power.tests_energy_breakdown import PowerEnergyBreakdown
from diff_tests.parser.power.tests_entity_state_residency import EntityStateResidency
from diff_tests.parser.power.tests_linux_sysfs_power import LinuxSysfsPower
from diff_tests.parser.power.tests_power_rails import PowerPowerRails
from diff_tests.parser.power.tests_voltage_and_scaling import PowerVoltageAndScaling
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
from diff_tests.stdlib.android.tests import AndroidStdlib
from diff_tests.stdlib.common.tests import StdlibCommon
from diff_tests.stdlib.chrome.tests import ChromeStdlib
from diff_tests.stdlib.chrome.tests_chrome_interactions import ChromeInteractions
from diff_tests.stdlib.chrome.tests_scroll_jank import ChromeScrollJankStdlib
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
      *AndroidBugreport(index_path, 'parser/android',
                        'AndroidBugreport').fetch(),
      *AndroidFs(index_path, 'parser/android_fs', 'AndroidFs').fetch(),
      *AndroidGames(index_path, 'parser/android', 'AndroidGames').fetch(),
      *AndroidParser(index_path, 'parser/android', 'AndroidParser').fetch(),
      *Atrace(index_path, 'parser/atrace', 'Atrace').fetch(),
      *AtraceErrorHandling(index_path, 'parser/atrace',
                           'AtraceErrorHandling').fetch(),
      *ChromeMemorySnapshots(index_path, 'parser/chrome',
                             'ChromeMemorySnapshots').fetch(),
      *ChromeParser(index_path, 'parser/chrome', 'ChromeParser').fetch(),
      *Cros(index_path, 'parser/cros', 'Cros').fetch(),
      *Fs(index_path, 'parser/fs', 'Fs').fetch(),
      *Fuchsia(index_path, 'parser/fuchsia', 'Fuchsia').fetch(),
      *GraphicsDrmRelatedFtraceEvents(index_path, 'parser/graphics',
                                      'GraphicsDrmRelatedFtraceEvents').fetch(),
      *GraphicsGpuTrace(index_path, 'parser/graphics',
                        'GraphicsGpuTrace').fetch(),
      *GraphicsParser(index_path, 'parser/graphics', 'GraphicsParser').fetch(),
      *MemoryParser(index_path, 'parser/memory', 'MemoryParser').fetch(),
      *NetworkParser(index_path, 'parser/network', 'NetworkParser').fetch(),
      *PowerEnergyBreakdown(index_path, 'parser/power',
                            'PowerEnergyBreakdown').fetch(),
      *PowerPowerRails(index_path, 'parser/power', 'PowerPowerRails').fetch(),
      *PowerVoltageAndScaling(index_path, 'parser/power',
                              'PowerVoltageAndScaling').fetch(),
      *EntityStateResidency(index_path, 'parser/power',
                            'EntityStateResidency').fetch(),
      *LinuxSysfsPower(index_path, 'parser/power', 'LinuxSysfsPower').fetch(),
      *ProcessTracking(index_path, 'parser/process_tracking',
                       'ProcessTracking').fetch(),
      *Profiling(index_path, 'parser/profiling', 'Profiling').fetch(),
      *ProfilingHeapGraph(index_path, 'parser/profiling',
                          'ProfilingHeapGraph').fetch(),
      *ProfilingHeapProfiling(index_path, 'parser/profiling',
                              'ProfilingHeapProfiling').fetch(),
      *ProfilingLlvmSymbolizer(index_path, 'parser/profiling',
                               'ProfilingLlvmSymbolizer').fetch(),
      *SchedParser(index_path, 'parser/sched', 'SchedParser').fetch(),
      *Smoke(index_path, 'parser/smoke', 'Smoke').fetch(),
      *SmokeComputeMetrics(index_path, 'parser/smoke',
                           'SmokeComputeMetrics').fetch(),
      *SmokeJson(index_path, 'parser/smoke', 'SmokeJson').fetch(),
      *SmokeSchedEvents(index_path, 'parser/smoke', 'SmokeSchedEvents').fetch(),
      *SurfaceFlingerLayers(index_path, 'parser/android',
                            'SurfaceFlingerLayers').fetch(),
      *SurfaceFlingerTransactions(index_path, 'parser/android',
                                  'SurfaceFlingerTransactions').fetch(),
      *TrackEvent(index_path, 'parser/track_event', 'TrackEvent').fetch(),
      *TranslatedArgs(index_path, 'parser/translated_args',
                      'TranslatedArgs').fetch(),
      *Ufs(index_path, 'parser/ufs', 'Ufs').fetch(),
      # TODO(altimin, lalitm): "parsing" should be split into more specific
      # directories.
      *Parsing(index_path, 'parser/parsing', 'Parsing').fetch(),
      *ParsingDebugAnnotation(index_path, 'parser/parsing',
                              'ParsingDebugAnnotation').fetch(),
      *ParsingRssStats(index_path, 'parser/parsing', 'ParsingRssStats').fetch(),
      *ParsingMemoryCounters(index_path, 'parser/parsing',
                             'ParsingMemoryCounters').fetch(),
  ]

  metrics_tests = [
      *AndroidMetrics(index_path, 'metrics/android', 'AndroidMetrics').fetch(),
      *Camera(index_path, 'metrics/camera', 'Camera').fetch(),
      *ChromeArgs(index_path, 'metrics/chrome', 'ChromeArgs').fetch(),
      *ChromeMetrics(index_path, 'metrics/chrome', 'ChromeMetrics').fetch(),
      *ChromeProcesses(index_path, 'metrics/chrome', 'ChromeProcesses').fetch(),
      *ChromeRailModes(index_path, 'metrics/chrome', 'ChromeRailModes').fetch(),
      *ChromeScrollJankMetrics(index_path, 'metrics/chrome',
                               'ChromeScrollJankMetrics').fetch(),
      *ChromeTouchGesture(index_path, 'metrics/chrome',
                          'ChromeTouchGesture').fetch(),
      *Codecs(index_path, 'metrics/codecs', 'Codecs').fetch(),
      *FrameTimeline(index_path, 'metrics/frame_timeline',
                     'FrameTimeline').fetch(),
      *GraphicsMetrics(index_path, 'metrics/graphics',
                       'GraphicsMetrics').fetch(),
      *IRQ(index_path, 'metrics/irq', 'IRQ').fetch(),
      *MemoryMetrics(index_path, 'metrics/memory', 'MemoryMetrics').fetch(),
      *NetworkMetrics(index_path, 'metrics/network', 'NetworkMetrics').fetch(),
      *Power(index_path, 'metrics/power', 'Power').fetch(),
      *ProfilingMetrics(index_path, 'metrics/profiling',
                        'ProfilingMetrics').fetch(),
      *Startup(index_path, 'metrics/startup', 'Startup').fetch(),
      *StartupBroadcasts(index_path, 'metrics/startup',
                         'StartupBroadcasts').fetch(),
      *StartupLockContention(index_path, 'metrics/startup',
                             'StartupLockContention').fetch(),
      *StartupMetrics(index_path, 'metrics/startup', 'StartupMetrics').fetch(),
      *WebView(index_path, 'metrics/webview', 'WebView').fetch(),
  ]

  stdlib_tests = [
      *AndroidStdlib(index_path, 'stdlib/android', 'AndroidStdlib').fetch(),
      *ChromeInteractions(index_path, 'stdlib/chrome',
                                      'ChromeInteractions').fetch(),
      *ChromeScrollJankStdlib(index_path, 'stdlib/chrome',
                              'ChromeScrollJankStdlib').fetch(),
      *ChromeStdlib(index_path, 'stdlib/chrome', 'ChromeStdlib').fetch(),
      *DynamicTables(index_path, 'stdlib/dynamic_tables',
                     'DynamicTables').fetch(),
      *Pkvm(index_path, 'stdlib/pkvm', 'Pkvm').fetch(),
      *StdlibCommon(index_path, 'stdlib/common', 'StdlibCommon').fetch(),
      *Slices(index_path, 'stdlib/slices', 'Slices').fetch(),
      *SpanJoinLeftJoin(index_path, 'stdlib/span_join',
                        'SpanJoinLeftJoin').fetch(),
      *SpanJoinOuterJoin(index_path, 'stdlib/span_join',
                         'SpanJoinOuterJoin').fetch(),
      *SpanJoinRegression(index_path, 'stdlib/span_join',
                          'SpanJoinRegression').fetch(),
      *SpanJoinSmoke(index_path, 'stdlib/span_join', 'SpanJoinSmoke').fetch(),
      *Timestamps(index_path, 'stdlib/timestamps', 'Timestamps').fetch(),
  ]

  syntax_tests = [
      *Functions(index_path, 'syntax/functions', 'Functions').fetch(),
      *PerfettoSql(index_path, 'syntax/perfetto_sql', 'PerfettoSql').fetch(),
  ]

  return parser_tests + metrics_tests + stdlib_tests + syntax_tests + [
      *Tables(index_path, 'tables', 'Tables').fetch(),
      *TablesCounters(index_path, 'tables', 'TablesCounters').fetch(),
      *TablesSched(index_path, 'tables', 'TablesSched').fetch(),
  ]
