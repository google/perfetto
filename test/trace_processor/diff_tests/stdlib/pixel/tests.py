#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PixelStdlib(TestSuite):

  def test_android_camera_frames(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet { ftrace_events {
          cpu: 0
          previous_bundle_end_timestamp: 2000
          event {
            timestamp: 2200
            pid: 42
            print { buf: "B|42|cam1_filter:output (frame 123)\n" }
          }
          event {
            timestamp: 2700
            pid: 42
            print { buf: "E|42\n" }
          }
        }}
        """),
        query="""
        INCLUDE PERFETTO MODULE pixel.camera;

        SELECT
          ts,
          node,
          port_group,
          frame_number,
          cam_id,
          dur
        FROM pixel_camera_frames
        ORDER BY ts
        """,
        out=Csv("""
        "ts","node","port_group","frame_number","cam_id","dur"
        2200,"filter","output",123,1,500
        """))

  def test_pixel_touch_events(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              machine: "x86_64"
            }
          }
        }
        packet {
          process_tree {
            processes {
              pid: 2
              ppid: 0
              cmdline: "kthreadd"
            }
            processes {
              pid: 30724
              ppid: 2
              cmdline: "irq/764-touch_dev"
            }
            processes {
              pid: 9876
              ppid: 1
              cmdline: "twoshay"
            }
            threads {
              tid: 30724
              tgid: 30724
              name: "irq/764-touch_dev"
            }
            threads {
              tid: 9876
              tgid: 9876
              name: "twoshay"
            }
          }
        }
        # ==============================================================================
        # CASE 1: Happy path - complete touch event sequence (TH IRQ -> BH Thread -> Twoshay)
        # ==============================================================================
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 280000
              pid: 0
              irq_handler_entry {
                irq: 764
                name: "touch_dev"
              }
            }
            event {
              timestamp: 282600
              pid: 0
              irq_handler_exit {
                irq: 764
                ret: 1
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            # Counter event (value: 98756)
            event {
              timestamp: 281300
              pid: 0
              zero {
                flag: 4 # Counter
                name: "gti_th_irq_index"
                pid: 0
                value: 98756
              }
            }
            # BH parent starts: gti_irq_thread_fn: IRQ_IDX=98756.
            event {
              timestamp: 305000
              pid: 30724
              zero {
                flag: 1 # Begin
                name: "gti_irq_thread_fn: IRQ_IDX=98756."
                pid: 0
                value: 0
              }
            }
            # BH child: goog_offload_populate_frame: IDX=98570 IN_TS=0.
            event {
              timestamp: 2290000
              pid: 30724
              zero {
                flag: 1 # Begin
                name: "goog_offload_populate_frame: IDX=98570 IN_TS=0."
                pid: 0
                value: 0
              }
            }
            # BH child ends
            event {
              timestamp: 2310000
              pid: 30724
              zero {
                flag: 2 # End
                name: ""
                pid: 0
                value: 0
              }
            }
            # BH parent ends
            event {
              timestamp: 2325000
              pid: 30724
              zero {
                flag: 2 # End
                name: ""
                pid: 0
                value: 0
              }
            }
            # Twoshay starts: algo->processFrame: INDEX=98570 IN_TS=0.
            event {
              timestamp: 2505000
              pid: 9876
              print {
                buf: "B|9876|algo->processFrame: INDEX=98570 IN_TS=0.\n"
              }
            }
            # Resample latency offset counter
            event {
              timestamp: 2600000
              pid: 9876
              zero {
                flag: 4 # Counter
                name: "Resample latency offset"
                pid: 9876
                value: 5000000
              }
            }
            # Twoshay ends
            event {
              timestamp: 2955000
              pid: 9876
              print {
                buf: "E|9876\n"
              }
            }
          }
        }

        # ==============================================================================
        # CASE 2: Missing TH only (ts: 10000000 + offsets)
        # - Twoshay IN_TS = 10000000
        # - BH IN_TS = 10000000
        # - No TH IRQ provided
        # ==============================================================================
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 10305000
              pid: 30724
              zero {
                flag: 1 # Begin
                name: "gti_irq_thread_fn: IRQ_IDX=98757."
                pid: 0
                value: 0
              }
            }
            event {
              timestamp: 12290000
              pid: 30724
              zero {
                flag: 1 # Begin
                name: "goog_offload_populate_frame: IDX=98571 IN_TS=10000000."
                pid: 0
                value: 0
              }
            }
            event {
              timestamp: 12310000
              pid: 30724
              zero {
                flag: 2 # End
                name: ""
                pid: 0
                value: 0
              }
            }
            event {
              timestamp: 12325000
              pid: 30724
              zero {
                flag: 2 # End
                name: ""
                pid: 0
                value: 0
              }
            }
            event {
              timestamp: 12505000
              pid: 9876
              print {
                buf: "B|9876|algo->processFrame: INDEX=98571 IN_TS=10000000.\n"
              }
            }
            event {
              timestamp: 12955000
              pid: 9876
              print {
                buf: "E|9876\n"
              }
            }
          }
        }

        # ==============================================================================
        # CASE 3: Missing BH and TH (ts: 20000000 + offsets)
        # - Only Twoshay event is present
        # ==============================================================================
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 22505000
              pid: 9876
              print {
                buf: "B|9876|algo->processFrame: INDEX=98572 IN_TS=20000000.\n"
              }
            }
            event {
              timestamp: 22955000
              pid: 9876
              print {
                buf: "E|9876\n"
              }
            }
          }
        }

        # ==============================================================================
        # CASE 4: Counter Outside TH Boundary (ts: 30000000 + offsets)
        # - TH IRQ is at 30280000 to 30282600
        # - Counter event is at 30000000 (before TH starts) -> Should NOT join
        # ==============================================================================
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 30280000
              pid: 0
              irq_handler_entry {
                irq: 764
                name: "touch_dev"
              }
            }
            event {
              timestamp: 30282600
              pid: 0
              irq_handler_exit {
                irq: 764
                ret: 1
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 30000000
              pid: 0
              zero {
                flag: 4 # Counter
                name: "gti_th_irq_index"
                pid: 0
                value: 98758
              }
            }
            event {
              timestamp: 30305000
              pid: 30724
              zero {
                flag: 1 # Begin
                name: "gti_irq_thread_fn: IRQ_IDX=98758."
                pid: 0
                value: 0
              }
            }
            event {
              timestamp: 32290000
              pid: 30724
              zero {
                flag: 1 # Begin
                name: "goog_offload_populate_frame: IDX=98573 IN_TS=30000000."
                pid: 0
                value: 0
              }
            }
            event {
              timestamp: 32310000
              pid: 30724
              zero {
                flag: 2 # End
                name: ""
                pid: 0
                value: 0
              }
            }
            event {
              timestamp: 32325000
              pid: 30724
              zero {
                flag: 2 # End
                name: ""
                pid: 0
                value: 0
              }
            }
            event {
              timestamp: 32505000
              pid: 9876
              print {
                buf: "B|9876|algo->processFrame: INDEX=98573 IN_TS=30000000.\n"
              }
            }
            event {
              timestamp: 32955000
              pid: 9876
              print {
                buf: "E|9876\n"
              }
            }
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE pixel.touch;

        SELECT
          in_ts,
          ts_pixel_touch_th,
          dur_pixel_touch_th,
          ts_pixel_touch_bh,
          dur_pixel_touch_bh,
          ts_pixel_touch,
          dur_pixel_touch,
          resample_latency_offset
        FROM pixel_touch_events
        ORDER BY ts_pixel_touch;
        """,
        out=Csv("""
        "in_ts","ts_pixel_touch_th","dur_pixel_touch_th","ts_pixel_touch_bh","dur_pixel_touch_bh","ts_pixel_touch","dur_pixel_touch","resample_latency_offset"
        0,280000,2600,305000,2020000,2505000,450000,5000000
        10000000,"[NULL]","[NULL]",10305000,2020000,12505000,450000,0
        20000000,"[NULL]","[NULL]","[NULL]","[NULL]",22505000,450000,0
        30000000,"[NULL]","[NULL]",30305000,2020000,32505000,450000,0
        """))
