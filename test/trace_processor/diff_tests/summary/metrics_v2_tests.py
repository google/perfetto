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

from python.generators.diff_tests.testing import DataPath, Path
from python.generators.diff_tests.testing import MetricV2SpecTextproto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class SummaryMetricsV2(TestSuite):

  def test_smoke_metric_v2(self):
    return DiffTestBlueprint(
        trace=DataPath('android_postboot_unlock.pftrace'),
        query=MetricV2SpecTextproto('''
          id: "memory_per_process"
          dimensions: "process_name"
          value: "avg_rss_and_swap"
          query: {
            table: {
              table_name: "memory_rss_and_swap_per_process"
              module_name: "linux.memory.process"
            }
            group_by: {
              column_names: "process_name"
              aggregates: {
                column_name: "rss_and_swap"
                op: DURATION_WEIGHTED_MEAN
                result_column_name: "avg_rss_and_swap"
              }
            }
          }
        '''),
        out=Path('smoke_metric_v2.out'))
