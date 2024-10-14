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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class GraphicsMetrics(TestSuite):
  def test_frame_missed_metrics(self):
    return DiffTestBlueprint(
        trace=Path('frame_missed.py'),
        query=Metric('android_surfaceflinger'),
        out=TextProto(r"""
        android_surfaceflinger {
          missed_frames: 3
          missed_hwc_frames: 0
          missed_gpu_frames: 0
          missed_frame_rate: 0.42857142857142855 # = 3/7
          gpu_invocations: 0
          metrics_per_display: {
            display_id: "101"
            missed_frames: 2
            missed_hwc_frames: 0
            missed_gpu_frames: 0
            missed_frame_rate: 0.5
          }
          metrics_per_display: {
            display_id: "102"
            missed_frames: 1
            missed_hwc_frames: 0
            missed_gpu_frames: 0
            missed_frame_rate: 0.33333333333333333
          }
        }
        """))

  def test_surfaceflinger_gpu_invocation(self):
    return DiffTestBlueprint(
        trace=Path('surfaceflinger_gpu_invocation.py'),
        query=Metric('android_surfaceflinger'),
        out=TextProto(r"""
        android_surfaceflinger {
          missed_frames: 0
          missed_hwc_frames: 0
          missed_gpu_frames: 0
          gpu_invocations: 4
          avg_gpu_waiting_dur_ms: 4
          total_non_empty_gpu_waiting_dur_ms: 11
        }
        """))

  # GPU metrics
  def test_gpu_metric(self):
    return DiffTestBlueprint(
        trace=Path('gpu_metric.py'),
        query=Metric('android_gpu'),
        out=TextProto(r"""
        android_gpu {
          processes {
            name: "app_1"
            mem_max: 8
            mem_min: 2
            mem_avg: 3
          }
          processes {
            name: "app_2"
            mem_max: 10
            mem_min: 6
            mem_avg: 8
          }
          mem_max: 4
          mem_min: 1
          mem_avg: 1
        }
        """))

  def test_gpu_frequency_metric(self):
    return DiffTestBlueprint(
        trace=Path('gpu_frequency_metric.textproto'),
        query=Metric('android_gpu'),
        out=Path('gpu_frequency_metric.out'))

  # Android Jank CUJ metric
  def test_android_jank_cuj(self):
    return DiffTestBlueprint(
        trace=Path('android_jank_cuj.py'),
        query=Metric('android_jank_cuj'),
        out=Path('android_jank_cuj.out'))

  def test_android_jank_cuj_query(self):
    return DiffTestBlueprint(
        trace=Path('android_jank_cuj.py'),
        query=Path('android_jank_cuj_query_test.sql'),
        out=Path('android_jank_cuj_query.out'))

  # Composition layer
  def test_composition_layer_count(self):
    return DiffTestBlueprint(
        trace=Path('composition_layer.py'),
        query="""
        SELECT RUN_METRIC('android/android_hwcomposer.sql');

        SELECT display_id, AVG(value)
        FROM total_layers
        GROUP BY display_id;
        """,
        out=Csv("""
        "display_id","AVG(value)"
        "0",3.000000
        "1",5.000000
        """))

  # G2D metrics TODO(rsavitski): find a real trace and double-check that the
  # is realistic. One kernel's source I checked had tgid=0 for all counter
  # Initial support was added/discussed in b/171296908.
  def test_g2d_metrics(self):
    return DiffTestBlueprint(
        trace=Path('g2d_metrics.textproto'),
        query=Metric('g2d'),
        out=Path('g2d_metrics.out'))

  # Composer execution
  def test_composer_execution(self):
    return DiffTestBlueprint(
        trace=Path('composer_execution.py'),
        query="""
        SELECT RUN_METRIC('android/composer_execution.sql',
          'output', 'hwc_execution_spans');

        SELECT
          validation_type,
          display_id,
          COUNT(*) AS count,
          SUM(execution_time_ns) AS total
        FROM hwc_execution_spans
        GROUP BY validation_type, display_id
        ORDER BY validation_type, display_id;
        """,
        out=Csv("""
        "validation_type","display_id","count","total"
        "separated_validation","1",1,200
        "skipped_validation","0",2,200
        "skipped_validation","1",1,100
        "unknown","1",1,0
        "unskipped_validation","0",1,200
        """))

  # Display metrics
  def test_display_metrics(self):
    return DiffTestBlueprint(
        trace=Path('display_metrics.py'),
        query=Metric('display_metrics'),
        out=TextProto(r"""
        display_metrics {
          total_duplicate_frames: 0
          duplicate_frames_logged: 0
          total_dpu_underrun_count: 0
          refresh_rate_switches: 5
          refresh_rate_stats {
            refresh_rate_fps: 60
            count: 2
            total_dur_ms: 2
            avg_dur_ms: 1
          }
          refresh_rate_stats {
            refresh_rate_fps: 90
            count: 2
            total_dur_ms: 2
            avg_dur_ms: 1
          }
          refresh_rate_stats {
            refresh_rate_fps: 120
            count: 1
            total_dur_ms: 2
            avg_dur_ms: 2
          }
          update_power_state {
            avg_runtime_micro_secs: 4000
          }
        }
        """))

  # DPU vote clock and bandwidth
  def test_dpu_vote_clock_bw(self):
    return DiffTestBlueprint(
        trace=Path('dpu_vote_clock_bw.textproto'),
        query=Metric('android_hwcomposer'),
        out=TextProto(r"""
        android_hwcomposer {
          skipped_validation_count: 0
          unskipped_validation_count: 0
          separated_validation_count: 0
          unknown_validation_count: 0
          dpu_vote_metrics {
            tid: 237
            avg_dpu_vote_clock: 206250
            avg_dpu_vote_avg_bw: 210000
            avg_dpu_vote_peak_bw: 205000
            avg_dpu_vote_rt_bw: 271000
          }
          dpu_vote_metrics {
            tid: 299
            avg_dpu_vote_clock: 250000
          }
        }
        """))