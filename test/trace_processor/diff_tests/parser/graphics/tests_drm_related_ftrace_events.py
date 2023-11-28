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


class GraphicsDrmRelatedFtraceEvents(TestSuite):

  def test_drm_vblank_gpu_track(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 6159770881976
              pid: 0
              drm_vblank_event {
                crtc: 0
                high_prec: 1
                seq: 3551
                time: 6159771267407
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 4
            event {
              timestamp: 6159770993376
              pid: 144
              drm_vblank_event_delivered {
                crtc: 0
                file: 18446743526216291840
                seq: 3551
              }
            }
          }
        }
        """),
        query="""
        SELECT
          gpu_track.name,
          ts,
          dur,
          slice.name,
          flat_key,
          int_value,
          string_value
        FROM
          gpu_track
        JOIN slice
          ON slice.track_id = gpu_track.id
        JOIN args
          ON slice.arg_set_id = args.arg_set_id
        ORDER BY ts;
        """,
        out=Csv("""
        "name","ts","dur","name","flat_key","int_value","string_value"
        "vblank-0",6159770881976,0,"signal","vblank seqno",3551,"[NULL]"
        "vblank-0",6159770993376,0,"deliver","vblank seqno",3551,"[NULL]"
        """))

  def test_drm_sched_gpu_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_sched.textproto'),
        query="""
        SELECT
          gpu_track.name,
          ts,
          dur,
          slice.name,
          flat_key,
          int_value,
          string_value
        FROM
          gpu_track
        JOIN slice
          ON slice.track_id = gpu_track.id
        JOIN args
          ON slice.arg_set_id = args.arg_set_id
        ORDER BY ts;
        """,
        out=Csv("""
        "name","ts","dur","name","flat_key","int_value","string_value"
        "sched-ring0",9246165349383,4729073,"job","gpu sched job",13481,"[NULL]"
        "sched-ring0",9246170078456,3941571,"job","gpu sched job",13482,"[NULL]"
        "sched-ring0",9246174020027,25156,"job","gpu sched job",13483,"[NULL]"
        "sched-ring0",9246181933273,4726312,"job","gpu sched job",13484,"[NULL]"
        """))

  def test_drm_sched_thread_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_sched.textproto'),
        query="""
        SELECT
          utid,
          ts,
          dur,
          slice.name,
          flat_key,
          int_value,
          string_value
        FROM
          thread_track
        JOIN slice
          ON slice.track_id = thread_track.id
        JOIN args
          ON slice.arg_set_id = args.arg_set_id
        ORDER BY ts;
        """,
        out=Csv("""
        "utid","ts","dur","name","flat_key","int_value","string_value"
        1,9246165326050,0,"drm_sched_job","gpu sched ring","[NULL]","ring0"
        1,9246165326050,0,"drm_sched_job","gpu sched job",13481,"[NULL]"
        3,9246166957616,0,"drm_sched_job","gpu sched ring","[NULL]","ring0"
        3,9246166957616,0,"drm_sched_job","gpu sched job",13482,"[NULL]"
        3,9246167272512,0,"drm_sched_job","gpu sched ring","[NULL]","ring0"
        3,9246167272512,0,"drm_sched_job","gpu sched job",13483,"[NULL]"
        1,9246181907439,0,"drm_sched_job","gpu sched ring","[NULL]","ring0"
        1,9246181907439,0,"drm_sched_job","gpu sched job",13484,"[NULL]"
        """))

  def test_drm_dma_fence_gpu_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_dma_fence.textproto'),
        query="""
        SELECT
          gpu_track.name,
          ts,
          dur,
          slice.name,
          flat_key,
          int_value,
          string_value
        FROM
          gpu_track
        JOIN slice
          ON slice.track_id = gpu_track.id
        JOIN args
          ON slice.arg_set_id = args.arg_set_id
        ORDER BY ts;
        """,
        out=Csv("""
        "name","ts","dur","name","flat_key","int_value","string_value"
        "fence-gpu-ring-0-1",11303602488073,12813,"fence","fence seqno",16665,"[NULL]"
        "fence-gpu-ring-0-1",11303602500886,4805626,"fence","fence seqno",16665,"[NULL]"
        "fence-gpu-ring-0-1",11303607306512,3850783,"fence","fence seqno",16666,"[NULL]"
        "fence-ring0-9",11303702681699,4868387,"fence","fence seqno",5065,"[NULL]"
        """))

  def test_drm_dma_fence_thread_track(self):
    return DiffTestBlueprint(
        trace=Path('drm_dma_fence.textproto'),
        query="""
        SELECT
          utid,
          ts,
          dur,
          slice.name,
          flat_key,
          int_value,
          string_value
        FROM
          thread_track
        JOIN slice
          ON slice.track_id = thread_track.id
        JOIN args
          ON slice.arg_set_id = args.arg_set_id
        ORDER BY ts;
        """,
        out=Csv("""
        "utid","ts","dur","name","flat_key","int_value","string_value"
        3,11303702851231,4867658,"dma_fence_wait","fence context",9,"[NULL]"
        3,11303702851231,4867658,"dma_fence_wait","fence seqno",5065,"[NULL]"
        """))
