#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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
import os
import sys
from typing import List, Tuple

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
from diff_tests.metrics.chrome.tests_scroll_jank import ChromeScrollJankMetrics
from diff_tests.metrics.codecs.tests import Codecs
from diff_tests.metrics.common.tests import CloneDurationMetrics
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
from diff_tests.parser.android.tests_cpu_per_uid import AndroidCpuPerUid
from diff_tests.parser.android.tests_dumpstate import AndroidDumpstate
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
from diff_tests.parser.art_hprof.tests import ArtHprofParser
from diff_tests.parser.art_method.tests import ArtMethodParser
from diff_tests.parser.atrace.tests import Atrace
from diff_tests.parser.atrace.tests_error_handling import AtraceErrorHandling
from diff_tests.parser.chrome.tests import ChromeParser
from diff_tests.parser.chrome.tests_memory_snapshots import ChromeMemorySnapshots
from diff_tests.parser.chrome.tests_v8 import ChromeV8Parser
from diff_tests.parser.cros.tests import Cros
from diff_tests.parser.etm.tests import Etm
from diff_tests.parser.etw.tests import Etw
from diff_tests.parser.fs.tests import Fs
from diff_tests.parser.ftrace.block_io_tests import BlockIo
from diff_tests.parser.ftrace.ftrace_crop_tests import FtraceCrop
from diff_tests.parser.ftrace.kprobes_tests import Kprobes
from diff_tests.parser.ftrace.generic_ftrace_tests import GenericFtrace
from diff_tests.parser.ftrace.kernel_trackevent_tests import KernelTrackevent
from diff_tests.parser.fuchsia.tests import Fuchsia
from diff_tests.parser.gecko.tests import GeckoParser
from diff_tests.parser.generic_kernel.tests import GenericKernelParser
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
from diff_tests.parser.pprof.tests_pprof import PprofParser
from diff_tests.parser.power.tests_battery_stats import BatteryStats
from diff_tests.parser.power.tests_energy_breakdown import PowerEnergyBreakdown
from diff_tests.parser.power.tests_entity_state_residency import EntityStateResidency
from diff_tests.parser.power.tests_linux_sysfs_power import LinuxSysfsPower
from diff_tests.parser.power.tests_power_rails import PowerPowerRails
from diff_tests.parser.power.tests_voltage_and_scaling import PowerVoltageAndScaling
from diff_tests.parser.process_tracking.tests import ProcessTracking
from diff_tests.parser.profiling.deobfuscation_tests import Deobfuscation
from diff_tests.parser.profiling.r8_retrace_compat.tests import R8RetraceCompat
from diff_tests.parser.profiling.tests import Profiling
from diff_tests.parser.profiling.tests_heap_graph import ProfilingHeapGraph
from diff_tests.parser.profiling.tests_heap_profiling import ProfilingHeapProfiling
from diff_tests.parser.profiling.tests_llvm_symbolizer import ProfilingLlvmSymbolizer
from diff_tests.parser.sched.tests import SchedParser
from diff_tests.parser.simpleperf.tests import Simpleperf
from diff_tests.parser.simpleperf_proto.tests import SimpleperfProtoParser
from diff_tests.parser.smoke.tests import Smoke
from diff_tests.parser.smoke.tests_compute_metrics import SmokeComputeMetrics
from diff_tests.parser.smoke.tests_json import SmokeJson
from diff_tests.parser.smoke.tests_sched_events import SmokeSchedEvents
from diff_tests.parser.track_event.tests import TrackEvent
from diff_tests.parser.translated_args.tests import TranslatedArgs
from diff_tests.parser.ufs.tests import Ufs
from diff_tests.parser.zip.tests import Zip
from diff_tests.stdlib.android.cpu_cluster_tests import CpuClusters
from diff_tests.stdlib.android.battery_tests import Battery
from diff_tests.stdlib.android.desktop_mode_tests import DesktopMode
from diff_tests.stdlib.android.frames_tests import Frames
from diff_tests.stdlib.android.gpu import AndroidGpu
from diff_tests.stdlib.android.heap_graph_tests import HeapGraph
from diff_tests.stdlib.android.heap_profile_tests import HeapProfile
from diff_tests.stdlib.prelude.unhex import UnHex
from diff_tests.stdlib.android.memory import AndroidMemory
from diff_tests.stdlib.android.network_packets import AndroidNetworkPackets
from diff_tests.stdlib.android.startups_tests import Startups
from diff_tests.stdlib.android.sysui_cujs_test import SystemUICujs
from diff_tests.stdlib.android.bitmaps import AndroidBitmaps
from diff_tests.stdlib.android.tests import AndroidStdlib
from diff_tests.stdlib.chrome.chrome_stdlib_testsuites import CHROME_STDLIB_TESTSUITES
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
from diff_tests.stdlib.pixel.tests import PixelStdlib
from diff_tests.stdlib.pkvm.tests import Pkvm
from diff_tests.stdlib.prelude.args_functions_tests import ArgsFunctions
from diff_tests.stdlib.prelude.math_functions_tests import PreludeMathFunctions
from diff_tests.stdlib.prelude.package_lookup_tests import PackageLookup
from diff_tests.stdlib.prelude.pprof_functions_tests import PreludePprofFunctions
from diff_tests.stdlib.prelude.regexp_extract import RegexpExtract
from diff_tests.stdlib.prelude.slices_tests import PreludeSlices
from diff_tests.stdlib.prelude.window_functions_tests import PreludeWindowFunctions
from diff_tests.stdlib.sched.tests import StdlibSched
from diff_tests.stdlib.slices.tests import Slices
from diff_tests.stdlib.slices.tests_stack import SlicesStack
from diff_tests.stdlib.span_join.tests_left_join import SpanJoinLeftJoin
from diff_tests.stdlib.span_join.tests_outer_join import SpanJoinOuterJoin
from diff_tests.stdlib.span_join.tests_regression import SpanJoinRegression
from diff_tests.stdlib.span_join.tests_smoke import SpanJoinSmoke
from diff_tests.stdlib.stacks.tests import Stacks
from diff_tests.stdlib.symbolize.tests import Symbolize
from diff_tests.stdlib.tests import StdlibSmoke
from diff_tests.stdlib.timestamps.tests import Timestamps
from diff_tests.stdlib.traced.stats import TracedStats
from diff_tests.stdlib.viz.tests import Viz
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
from diff_tests.summary.metrics_v2_tests import SummaryMetricsV2

sys.path.pop()


def fetch_all_diff_tests(
    index_path: str) -> List[Tuple[str, 'testing.DiffTestBlueprint']]:
  parser_tests = [
      AndroidBugreport,
      AndroidCpuPerUid,
      AndroidDumpstate,
      AndroidFs,
      AndroidGames,
      AndroidParser,
      Atrace,
      AtraceErrorHandling,
      ChromeMemorySnapshots,
      ChromeParser,
      ChromeV8Parser,
      Cros,
      Deobfuscation,
      R8RetraceCompat,
      Etm,
      Etw,
      Fs,
      Fuchsia,
      GenericFtrace,
      GenericKernelParser,
      GraphicsDrmRelatedFtraceEvents,
      GraphicsGpuTrace,
      GraphicsParser,
      JsonParser,
      KernelTrackevent,
      MemoryParser,
      NetworkParser,
      BatteryStats,
      PowerEnergyBreakdown,
      PowerPowerRails,
      PowerVoltageAndScaling,
      EntityStateResidency,
      LinuxSysfsPower,
      ProcessTracking,
      Profiling,
      ProfilingHeapGraph,
      ProfilingHeapProfiling,
      ProfilingLlvmSymbolizer,
      SchedParser,
      Simpleperf,
      SimpleperfProtoParser,
      StdlibSched,
      Smoke,
      SmokeComputeMetrics,
      SmokeJson,
      SmokeSchedEvents,
      Symbolize,
      InputMethodClients,
      InputMethodManagerService,
      InputMethodService,
      SurfaceFlingerLayers,
      SurfaceFlingerTransactions,
      ShellTransitions,
      ProtoLog,
      ViewCapture,
      WindowManager,
      TrackEvent,
      TranslatedArgs,
      Ufs,
      Parsing,
      ParsingDebugAnnotation,
      ParsingRssStats,
      ParsingSysStats,
      ParsingMemoryCounters,
      BlockIo,
      FtraceCrop,
      Kprobes,
      ParsingTracedStats,
      Zip,
      AndroidInputEvent,
      Instruments,
      Gzip,
      GeckoParser,
      ArtHprofParser,
      ArtMethodParser,
      PerfTextParser,
      PprofParser,
  ]

  metrics_tests = [
      AndroidMetrics,
      Camera,
      ChromeArgs,
      ChromeMetrics,
      ChromeProcesses,
      ChromeScrollJankMetrics,
      Codecs,
      FrameTimeline,
      GraphicsMetrics,
      IRQ,
      CloneDurationMetrics,
      MemoryMetrics,
      NetworkMetrics,
      Power,
      ProfilingMetrics,
      Startup,
      StartupBroadcasts,
      StartupLockContention,
      StartupMetrics,
      WebView,
  ]

  stdlib_tests = [
      AndroidMemory,
      AndroidNetworkPackets,
      AndroidGpu,
      AndroidStdlib,
      AndroidBitmaps,
      ArgsFunctions,
      CpuClusters,
      Battery,
      DesktopMode,
      LinuxCpu,
      LinuxTests,
      DominatorTree,
      CriticalPathTests,
      GraphScanTests,
      ExportTests,
      Frames,
      GraphSearchTests,
      GraphPartitionTests,
      StdlibCounterIntervals,
      DynamicTables,
      Memory,
      PackageLookup,
      PreludeMathFunctions,
      HeapGraph,
      UnHex,
      PreludePprofFunctions,
      PreludeWindowFunctions,
      RegexpExtract,
      Pkvm,
      PreludeSlices,
      StdlibSmoke,
      Slices,
      SlicesStack,
      SpanJoinLeftJoin,
      SpanJoinOuterJoin,
      SpanJoinRegression,
      SpanJoinSmoke,
      Stacks,
      StdlibIntervals,
      SystemUICujs,
      IntervalsIntersect,
      Startups,
      Timestamps,
      TracedStats,
      Viz,
      WattsonStdlib,
      HeapProfile,
      PixelStdlib,
  ]

  syntax_tests = [
      PerfettoFiltering,
      PerfettoFunction,
      PerfettoInclude,
      PerfettoMacro,
      PerfettoTable,
      PerfettoTableFunction,
      PerfettoView,
  ]

  tables_tests = [
      Tables,
      TablesCounters,
      TablesSched,
  ]

  summary_tests = [SummaryMetricsV2]

  all_tests = []
  all_tests += parser_tests
  all_tests += metrics_tests
  all_tests += stdlib_tests
  all_tests += syntax_tests
  all_tests += tables_tests
  all_tests += summary_tests
  all_test_instances = [x for t in all_tests for x in t(index_path).fetch()]

  # Chrome is special because it is rolled in from externally. So it has its
  # own segregated copy of everything.
  chrome_test_dir = os.path.abspath(
      os.path.join(__file__, '../../../data/chrome'))
  chrome_stdlib_tests_instances = []
  for test_suite_cls in CHROME_STDLIB_TESTSUITES:
    test_suite = test_suite_cls(index_path, chrome_test_dir)
    chrome_stdlib_tests_instances += test_suite.fetch()

  return all_test_instances + chrome_stdlib_tests_instances
