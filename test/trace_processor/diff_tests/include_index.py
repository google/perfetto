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
from diff_tests.parser.android_fs.tests import AndroidFs
from diff_tests.parser.android.tests import AndroidParser
from diff_tests.parser.android.tests_android_input_event import AndroidInputEvent
from diff_tests.parser.android.tests_bugreport import AndroidBugreport
from diff_tests.parser.android.tests_games import AndroidGames
from diff_tests.parser.android.tests_inputmethod_clients import InputMethodClients
from diff_tests.parser.android.tests_inputmethod_manager_service import InputMethodManagerService
from diff_tests.parser.android.tests_inputmethod_service import InputMethodService
from diff_tests.parser.android.tests_protolog import ProtoLog
from diff_tests.parser.android.tests_shell_transitions import ShellTransitions
from diff_tests.parser.android.tests_surfaceflinger_layers import SurfaceFlingerLayers
from diff_tests.parser.android.tests_surfaceflinger_transactions import SurfaceFlingerTransactions
from diff_tests.parser.android.tests_viewcapture import ViewCapture
from diff_tests.parser.android.tests_windowmanager import WindowManager
from diff_tests.parser.art_method.tests import ArtMethodParser
from diff_tests.parser.atrace.tests import Atrace
from diff_tests.parser.atrace.tests_error_handling import AtraceErrorHandling
from diff_tests.parser.chrome.tests import ChromeParser
from diff_tests.parser.chrome.tests_memory_snapshots import ChromeMemorySnapshots
from diff_tests.parser.chrome.tests_v8 import ChromeV8Parser
from diff_tests.parser.cros.tests import Cros
from diff_tests.parser.fs.tests import Fs
from diff_tests.parser.ftrace.ftrace_crop_tests import FtraceCrop
from diff_tests.parser.fuchsia.tests import Fuchsia
from diff_tests.parser.gecko.tests import GeckoParser
from diff_tests.parser.graphics.tests import GraphicsParser
from diff_tests.parser.graphics.tests_drm_related_ftrace_events import GraphicsDrmRelatedFtraceEvents
from diff_tests.parser.graphics.tests_gpu_trace import GraphicsGpuTrace
from diff_tests.parser.gzip.tests import Gzip
from diff_tests.parser.instruments.tests import Instruments
from diff_tests.parser.json.tests import JsonParser
from diff_tests.parser.memory.tests import MemoryParser
from diff_tests.parser.network.tests import NetworkParser
from diff_tests.parser.parsing.tests import Parsing
from diff_tests.parser.parsing.tests_debug_annotation import ParsingDebugAnnotation
from diff_tests.parser.parsing.tests_memory_counters import ParsingMemoryCounters
from diff_tests.parser.parsing.tests_rss_stats import ParsingRssStats
from diff_tests.parser.parsing.tests_sys_stats import ParsingSysStats
from diff_tests.parser.parsing.tests_traced_stats import ParsingTracedStats
from diff_tests.parser.perf_text.tests import PerfTextParser
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
from diff_tests.parser.simpleperf.tests import Simpleperf
from diff_tests.parser.smoke.tests import Smoke
from diff_tests.parser.smoke.tests_compute_metrics import SmokeComputeMetrics
from diff_tests.parser.smoke.tests_json import SmokeJson
from diff_tests.parser.smoke.tests_sched_events import SmokeSchedEvents
from diff_tests.parser.track_event.tests import TrackEvent
from diff_tests.parser.translated_args.tests import TranslatedArgs
from diff_tests.parser.ufs.tests import Ufs
from diff_tests.parser.zip.tests import Zip
from diff_tests.stdlib.android.cpu_cluster_tests import CpuClusters
from diff_tests.stdlib.android.frames_tests import Frames
from diff_tests.stdlib.android.gpu import AndroidGpu
from diff_tests.stdlib.android.heap_graph_tests import HeapGraph
from diff_tests.stdlib.android.memory import AndroidMemory
from diff_tests.stdlib.android.startups_tests import Startups
from diff_tests.stdlib.android.tests import AndroidStdlib
from diff_tests.stdlib.chrome.chrome_stdlib_testsuites import CHROME_STDLIB_TESTSUITES
from diff_tests.stdlib.common.tests import StdlibCommon
from diff_tests.stdlib.common.tests import StdlibCommon
from diff_tests.stdlib.counters.tests import StdlibCounterIntervals
from diff_tests.stdlib.dynamic_tables.tests import DynamicTables
from diff_tests.stdlib.export.tests import ExportTests
from diff_tests.stdlib.graphs.critical_path_tests import CriticalPathTests
from diff_tests.stdlib.graphs.dominator_tree_tests import DominatorTree
from diff_tests.stdlib.graphs.partition_tests import GraphPartitionTests
from diff_tests.stdlib.graphs.scan_tests import GraphScanTests
from diff_tests.stdlib.graphs.search_tests import GraphSearchTests
from diff_tests.stdlib.intervals.intersect_tests import IntervalsIntersect
from diff_tests.stdlib.intervals.tests import StdlibIntervals
from diff_tests.stdlib.linux.cpu import LinuxCpu
from diff_tests.stdlib.linux.memory import Memory
from diff_tests.stdlib.linux.tests import LinuxTests
from diff_tests.stdlib.pkvm.tests import Pkvm
from diff_tests.stdlib.prelude.math_functions_tests import PreludeMathFunctions
from diff_tests.stdlib.prelude.pprof_functions_tests import PreludePprofFunctions
from diff_tests.stdlib.prelude.slices_tests import PreludeSlices
from diff_tests.stdlib.prelude.window_functions_tests import PreludeWindowFunctions
from diff_tests.stdlib.sched.tests import StdlibSched
from diff_tests.stdlib.slices.tests import Slices
from diff_tests.stdlib.span_join.tests_left_join import SpanJoinLeftJoin
from diff_tests.stdlib.span_join.tests_outer_join import SpanJoinOuterJoin
from diff_tests.stdlib.span_join.tests_regression import SpanJoinRegression
from diff_tests.stdlib.span_join.tests_smoke import SpanJoinSmoke
from diff_tests.stdlib.tests import StdlibSmoke
from diff_tests.stdlib.timestamps.tests import Timestamps
from diff_tests.stdlib.wattson.tests import WattsonStdlib
from diff_tests.syntax.filtering_tests import PerfettoFiltering
from diff_tests.syntax.function_tests import PerfettoFunction
from diff_tests.syntax.include_tests import PerfettoInclude
from diff_tests.syntax.macro_tests import PerfettoMacro
from diff_tests.syntax.table_function_tests import PerfettoTableFunction
from diff_tests.syntax.table_tests import PerfettoTable
from diff_tests.syntax.view_tests import PerfettoView
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
      *ChromeV8Parser(index_path, 'parser/chrome', 'ChromeV8Parser').fetch(),
      *Cros(index_path, 'parser/cros', 'Cros').fetch(),
      *Fs(index_path, 'parser/fs', 'Fs').fetch(),
      *Fuchsia(index_path, 'parser/fuchsia', 'Fuchsia').fetch(),
      *GraphicsDrmRelatedFtraceEvents(index_path, 'parser/graphics',
                                      'GraphicsDrmRelatedFtraceEvents').fetch(),
      *GraphicsGpuTrace(index_path, 'parser/graphics',
                        'GraphicsGpuTrace').fetch(),
      *GraphicsParser(index_path, 'parser/graphics', 'GraphicsParser').fetch(),
      *JsonParser(index_path, 'parser/json', 'JsonParser').fetch(),
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
      *Simpleperf(index_path, 'parser/simpleperf', 'Simpleperf').fetch(),
      *StdlibSched(index_path, 'stdlib/sched', 'StdlibSched').fetch(),
      *Smoke(index_path, 'parser/smoke', 'Smoke').fetch(),
      *SmokeComputeMetrics(index_path, 'parser/smoke',
                           'SmokeComputeMetrics').fetch(),
      *SmokeJson(index_path, 'parser/smoke', 'SmokeJson').fetch(),
      *SmokeSchedEvents(index_path, 'parser/smoke', 'SmokeSchedEvents').fetch(),
      *InputMethodClients(index_path, 'parser/android',
                          'InputMethodClients').fetch(),
      *InputMethodManagerService(index_path, 'parser/android',
                                 'InputMethodManagerService').fetch(),
      *InputMethodService(index_path, 'parser/android',
                          'InputMethodService').fetch(),
      *SurfaceFlingerLayers(index_path, 'parser/android',
                            'SurfaceFlingerLayers').fetch(),
      *SurfaceFlingerTransactions(index_path, 'parser/android',
                                  'SurfaceFlingerTransactions').fetch(),
      *ShellTransitions(index_path, 'parser/android',
                        'ShellTransitions').fetch(),
      *ProtoLog(index_path, 'parser/android', 'ProtoLog').fetch(),
      *ViewCapture(index_path, 'parser/android', 'ViewCapture').fetch(),
      *WindowManager(index_path, 'parser/android', 'WindowManager').fetch(),
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
      *ParsingSysStats(index_path, 'parser/parsing', 'ParsingSysStats').fetch(),
      *ParsingMemoryCounters(index_path, 'parser/parsing',
                             'ParsingMemoryCounters').fetch(),
      *FtraceCrop(index_path, 'parser/ftrace', 'FtraceCrop').fetch(),
      *ParsingTracedStats(index_path, 'parser/parsing',
                          'ParsingTracedStats').fetch(),
      *Zip(index_path, 'parser/zip', 'Zip').fetch(),
      *AndroidInputEvent(index_path, 'parser/android',
                         'AndroidInputEvent').fetch(),
      *Instruments(index_path, 'parser/instruments', 'Instruments').fetch(),
      *Gzip(index_path, 'parser/gzip', 'Gzip').fetch(),
      *GeckoParser(index_path, 'parser/gecko', 'GeckoParser').fetch(),
      *ArtMethodParser(index_path, 'parser/art_method',
                       'ArtMethodParser').fetch(),
      *PerfTextParser(index_path, 'parser/perf_text', 'PerfTextParser').fetch(),
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
      *NetworkMetrics(index_path, 'metrics/network', 'orkMetrics').fetch(),
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

  chrome_test_dir = os.path.abspath(
      os.path.join(__file__, '../../../data/chrome'))
  chrome_stdlib_tests = []
  for test_suite_cls in CHROME_STDLIB_TESTSUITES:
    test_suite = test_suite_cls(index_path, 'stdlib/chrome',
                                test_suite_cls.__name__, chrome_test_dir)
    chrome_stdlib_tests += test_suite.fetch()

  stdlib_tests = [
      *AndroidMemory(index_path, 'stdlib/android', 'AndroidMemory').fetch(),
      *AndroidGpu(index_path, 'stdlib/android', 'AndroidGpu').fetch(),
      *AndroidStdlib(index_path, 'stdlib/android', 'AndroidStdlib').fetch(),
      *CpuClusters(index_path, 'stdlib/android', 'CpuClusters').fetch(),
      *LinuxCpu(index_path, 'stdlib/linux/cpu', 'LinuxCpu').fetch(),
      *LinuxTests(index_path, 'stdlib/linux', 'LinuxTests').fetch(),
      *DominatorTree(index_path, 'stdlib/graphs', 'DominatorTree').fetch(),
      *CriticalPathTests(index_path, 'stdlib/graphs', 'CriticalPath').fetch(),
      *GraphScanTests(index_path, 'stdlib/graphs', 'GraphScan').fetch(),
      *ExportTests(index_path, 'stdlib/export', 'ExportTests').fetch(),
      *Frames(index_path, 'stdlib/android', 'Frames').fetch(),
      *GraphSearchTests(index_path, 'stdlib/graphs',
                        'GraphSearchTests').fetch(),
      *GraphPartitionTests(index_path, 'stdlib/graphs',
                           'GraphPartitionTests').fetch(),
      *StdlibCounterIntervals(index_path, 'stdlib/counters',
                              'StdlibCounterIntervals').fetch(),
      *DynamicTables(index_path, 'stdlib/dynamic_tables',
                     'DynamicTables').fetch(),
      *Memory(index_path, 'stdlib/linux', 'Memory').fetch(),
      *PreludeMathFunctions(index_path, 'stdlib/prelude',
                            'PreludeMathFunctions').fetch(),
      *HeapGraph(index_path, 'stdlib/android',
                 'HeapGraphDominatorTree').fetch(),
      *PreludePprofFunctions(index_path, 'stdlib/prelude',
                             'PreludePprofFunctions').fetch(),
      *PreludeWindowFunctions(index_path, 'stdlib/prelude',
                              'PreludeWindowFunctions').fetch(),
      *Pkvm(index_path, 'stdlib/pkvm', 'Pkvm').fetch(),
      *PreludeSlices(index_path, 'stdlib/prelude', 'PreludeSlices').fetch(),
      *StdlibSmoke(index_path, 'stdlib', 'StdlibSmoke').fetch(),
      *StdlibCommon(index_path, 'stdlib/common', 'StdlibCommon').fetch(),
      *Slices(index_path, 'stdlib/slices', 'Slices').fetch(),
      *SpanJoinLeftJoin(index_path, 'stdlib/span_join',
                        'SpanJoinLeftJoin').fetch(),
      *SpanJoinOuterJoin(index_path, 'stdlib/span_join',
                         'SpanJoinOuterJoin').fetch(),
      *SpanJoinRegression(index_path, 'stdlib/span_join',
                          'SpanJoinRegression').fetch(),
      *SpanJoinSmoke(index_path, 'stdlib/span_join', 'SpanJoinSmoke').fetch(),
      *StdlibCommon(index_path, 'stdlib/common', 'StdlibCommon').fetch(),
      *StdlibIntervals(index_path, 'stdlib/intervals',
                       'StdlibIntervals').fetch(),
      *IntervalsIntersect(index_path, 'stdlib/intervals',
                          'StdlibIntervalsIntersect').fetch(),
      *Startups(index_path, 'stdlib/android', 'Startups').fetch(),
      *Timestamps(index_path, 'stdlib/timestamps', 'Timestamps').fetch(),
      *WattsonStdlib(index_path, 'stdlib/wattson', 'WattsonStdlib').fetch(),
  ] + chrome_stdlib_tests

  syntax_tests = [
      *PerfettoFiltering(index_path, 'syntax', 'PerfettoFiltering').fetch(),
      *PerfettoFunction(index_path, 'syntax', 'PerfettoFunction').fetch(),
      *PerfettoInclude(index_path, 'syntax', 'PerfettoInclude').fetch(),
      *PerfettoMacro(index_path, 'syntax', 'PerfettoMacro').fetch(),
      *PerfettoTable(index_path, 'syntax', 'PerfettoTable').fetch(),
      *PerfettoTableFunction(index_path, 'syntax',
                             'PerfettoTableFunction').fetch(),
      *PerfettoView(index_path, 'syntax', 'PerfettoView').fetch(),
  ]

  return parser_tests + metrics_tests + stdlib_tests + syntax_tests + [
      *Tables(index_path, 'tables', 'Tables').fetch(),
      *TablesCounters(index_path, 'tables', 'TablesCounters').fetch(),
      *TablesSched(index_path, 'tables', 'TablesSched').fetch(),
  ]
