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

from android.index import DiffTestModule_Android
from atrace.index import DiffTestModule_Atrace
from camera.index import DiffTestModule_Camera
from chrome.index import DiffTestModule_Chrome
from cros.index import DiffTestModule_Cros
from dynamic.index import DiffTestModule_Dynamic
from fs.index import DiffTestModule_Fs
from fuchsia.index import DiffTestModule_Fuchsia
from functions.index import DiffTestModule_Functions
from graphics.index import DiffTestModule_Graphics
from ufs.index import DiffTestModule_Ufs
from memory.index import DiffTestModule_Memory
from network.index import DiffTestModule_Network
from parsing.index import DiffTestModule_Parsing
from performance.index import DiffTestModule_Performance
from power.index import DiffTestModule_Power
from process_tracking.index import DiffTestModule_Process_tracking
from profiling.index import DiffTestModule_Profiling
from scheduler.index import DiffTestModule_Scheduler
from smoke.index import DiffTestModule_Smoke
from span_join.index import DiffTestModule_Span_join
from startup.index import DiffTestModule_Startup
from tables.index import DiffTestModule_Tables
from track_event.index import DiffTestModule_Track_event
from translation.index import DiffTestModule_Translation


def fetch_all_diff_tests(include_index_path: str) -> List['testing.DiffTest']:
  diff_tests = []
  diff_tests.extend(
      DiffTestModule_Android(include_index_path, 'android').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Atrace(include_index_path, 'atrace').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Camera(include_index_path, 'camera').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Chrome(include_index_path, 'chrome').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Cros(include_index_path, 'cros').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Dynamic(include_index_path, 'dynamic').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Fs(include_index_path, 'fs').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Fuchsia(include_index_path, 'fuchsia').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Functions(include_index_path,
                               'functions').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Graphics(include_index_path,
                              'graphics').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Ufs(include_index_path, 'ufs').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Memory(include_index_path, 'memory').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Network(include_index_path, 'network').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Parsing(include_index_path, 'parsing').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Performance(include_index_path,
                                 'performance').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Power(include_index_path, 'power').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Process_tracking(include_index_path,
                                      'process_tracking').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Profiling(include_index_path,
                               'profiling').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Scheduler(include_index_path,
                               'scheduler').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Smoke(include_index_path, 'smoke').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Span_join(include_index_path,
                               'span_join').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Startup(include_index_path, 'startup').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Tables(include_index_path, 'tables').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Track_event(include_index_path,
                                 'track_event').fetch_diff_tests())
  diff_tests.extend(
      DiffTestModule_Translation(include_index_path,
                                 'translation').fetch_diff_tests())
  return diff_tests
