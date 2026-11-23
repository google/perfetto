#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

# Synthetic trace exercising `android.frame_blocking_calls.instructions`:
# 1. Completed jank CUJ (`J<BACK_PANEL_ARROW>`) with two frames (20 and 22),
#    UI thread blocking calls (including an off-CPU sleeping binder transaction
#    at [30ms, 31ms] that must report 0.0 instructions), RenderThread
#    `DrawFrames`, and two `hwuiTask*` threads (`hwuiTask0` and `hwuiTask1`)
#    sharing one logical `call_id` in frame 20.
# 2. Out-of-CUJ sched and perf instruction spikes that must be excluded while
#    straddling boundary perf samples [20ms, 70ms] are preserved.
# 3. Canceled jank CUJ (`J<SHADE_EXPAND_COLLAPSE>`) that must be excluded.

import sys
import synth_common

SYSUI_PID = 5000
SYSUI_RTID = 1555
HWUI_TID_0 = 1556
HWUI_TID_1 = 1557
LAYER_1 = "TX - first_layer#0"

SW_CONTEXT_SWITCHES = 4
HW_INSTRUCTIONS = 11
PERF_SEQ_ID = 4


def add_frame_timeline(trace, ts, dur, token, pid):
  trace.add_expected_surface_frame_start_event(
      ts=ts,
      cookie=100000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=pid,
      layer_name="")
  trace.add_frame_end_event(ts=ts + 16_000_000, cookie=100000 + token)
  trace.add_actual_surface_frame_start_event(
      ts=ts,
      cookie=200000 + token,
      token=token,
      display_frame_token=100 + token,
      pid=pid,
      present_type=1,
      on_time_finish=1,
      gpu_composition=0,
      jank_type=1,
      prediction_type=3,
      layer_name=LAYER_1)
  trace.add_frame_end_event(ts=ts + dur, cookie=200000 + token)


def add_perf_sample_defaults(trace):
  pkt = trace.add_packet(ts=0)
  pkt.first_packet_on_sequence = True
  pkt.sequence_flags = 1
  pkt.trusted_packet_sequence_id = PERF_SEQ_ID
  perf_defaults = pkt.trace_packet_defaults.perf_sample_defaults
  perf_defaults.timebase.period = 1
  perf_defaults.timebase.counter = SW_CONTEXT_SWITCHES
  follower = perf_defaults.followers.add()
  follower.counter = HW_INSTRUCTIONS


def add_perf_instruction_sample(trace, ts, cpu, pid, tid, instructions):
  pkt = trace.add_packet(ts=ts)
  pkt.sequence_flags = 2
  pkt.trusted_packet_sequence_id = PERF_SEQ_ID
  pkt.perf_sample.cpu = cpu
  pkt.perf_sample.pid = pid
  pkt.perf_sample.tid = tid
  pkt.perf_sample.timebase_count = ts
  pkt.perf_sample.follower_counts.append(instructions)


trace = synth_common.create_trace()
trace.add_package_list(
    ts=0, name="com.android.systemui", uid=10001, version_code=1)
trace.add_packet()
trace.add_process(
    pid=SYSUI_PID, ppid=SYSUI_PID, cmdline="com.android.systemui", uid=10001)
trace.add_thread(
    tid=SYSUI_PID, tgid=SYSUI_PID, cmdline="MainThread", name="MainThread")
trace.add_thread(
    tid=SYSUI_RTID, tgid=SYSUI_PID, cmdline="RenderThread", name="RenderThread")
trace.add_thread(
    tid=HWUI_TID_0, tgid=SYSUI_PID, cmdline="hwuiTask0", name="hwuiTask0")
trace.add_thread(
    tid=HWUI_TID_1, tgid=SYSUI_PID, cmdline="hwuiTask1", name="hwuiTask1")

# --- CPU 0: CUJ markers, UI thread slices, and sched ---
trace.add_ftrace_packet(cpu=0)

completed_cuj = "J<BACK_PANEL_ARROW>"
trace.add_async_atrace_for_thread(
    ts=25_000_000,
    ts_end=50_000_000,
    buf=completed_cuj,
    pid=SYSUI_PID,
    tid=SYSUI_PID)
trace.add_atrace_instant(
    ts=25_000_001,
    buf=completed_cuj + "#UIThread",
    pid=SYSUI_PID,
    tid=SYSUI_PID)
trace.add_atrace_instant_for_track(
    ts=25_000_002,
    buf="FT#beginVsync#20",
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    track_name=completed_cuj)
trace.add_atrace_instant_for_track(
    ts=25_000_010,
    buf="FT#layerId#0",
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    track_name=completed_cuj)
trace.add_atrace_instant_for_track(
    ts=49_000_000,
    buf="FT#endVsync#30",
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    track_name=completed_cuj)

canceled_cuj = "J<SHADE_EXPAND_COLLAPSE>"
trace.add_async_atrace_for_thread(
    ts=55_000_000,
    ts_end=70_000_000,
    buf=canceled_cuj,
    pid=SYSUI_PID,
    tid=SYSUI_PID)
trace.add_atrace_instant(
    ts=55_000_001, buf=canceled_cuj + "#UIThread", pid=SYSUI_PID, tid=SYSUI_PID)
trace.add_atrace_instant_for_track(
    ts=55_000_002,
    buf="FT#beginVsync#40",
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    track_name=canceled_cuj)
trace.add_atrace_instant_for_track(
    ts=55_000_010,
    buf="FT#layerId#0",
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    track_name=canceled_cuj)
trace.add_atrace_instant_for_track(
    ts=69_000_000,
    buf="FT#cancel#0",
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    track_name=canceled_cuj)

# Out-of-CUJ sched [5ms, 15ms]; ignored by CUJ bounds.
trace.add_sched(
    ts=5_000_000, prev_pid=0, next_pid=SYSUI_PID, next_comm="MainThread")
trace.add_sched(
    ts=15_000_000,
    prev_pid=SYSUI_PID,
    next_pid=0,
    prev_comm="MainThread",
    prev_state="S")
# UI thread running on CPU 0 over [20ms, 30ms] and [31ms, 70ms]; sleeps off-CPU
# during [30ms, 31ms] so the second binder transaction gets 0.0 instructions.
trace.add_sched(
    ts=20_000_000, prev_pid=0, next_pid=SYSUI_PID, next_comm="MainThread")
trace.add_sched(
    ts=30_000_000,
    prev_pid=SYSUI_PID,
    next_pid=0,
    prev_comm="MainThread",
    prev_state="S")
trace.add_sched(
    ts=31_000_000, prev_pid=0, next_pid=SYSUI_PID, next_comm="MainThread")
trace.add_sched(
    ts=70_000_000,
    prev_pid=SYSUI_PID,
    next_pid=0,
    prev_comm="MainThread",
    prev_state="S")

# Frame 20 on UI thread.
trace.add_frame(
    vsync=20,
    ts_do_frame=26_000_000,
    ts_end_do_frame=32_000_000,
    tid=SYSUI_PID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=27_000_000,
    ts_end=29_000_000,
    buf="animation",
    tid=SYSUI_PID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=29_000_000,
    ts_end=30_000_000,
    buf="binder transaction",
    tid=SYSUI_PID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=30_000_000,
    ts_end=31_000_000,
    buf="binder transaction",
    tid=SYSUI_PID,
    pid=SYSUI_PID)

# Frame 22 on UI thread.
trace.add_frame(
    vsync=22,
    ts_do_frame=43_000_000,
    ts_end_do_frame=49_000_000,
    tid=SYSUI_PID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=44_000_000,
    ts_end=48_000_000,
    buf="animation",
    tid=SYSUI_PID,
    pid=SYSUI_PID)

# Frame 40 in canceled CUJ.
trace.add_frame(
    vsync=40,
    ts_do_frame=56_000_000,
    ts_end_do_frame=62_000_000,
    tid=SYSUI_PID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=57_000_000,
    ts_end=59_000_000,
    buf="animation",
    tid=SYSUI_PID,
    pid=SYSUI_PID)

# --- CPU 1: RenderThread slices and sched ---
trace.add_ftrace_packet(cpu=1)
trace.add_sched(
    ts=20_000_000, prev_pid=0, next_pid=SYSUI_RTID, next_comm="RenderThread")
trace.add_sched(
    ts=70_000_000,
    prev_pid=SYSUI_RTID,
    next_pid=0,
    prev_comm="RenderThread",
    prev_state="S")
trace.add_atrace_for_thread(
    ts=28_000_000,
    ts_end=30_000_000,
    buf="DrawFrames 20",
    tid=SYSUI_RTID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=45_000_000,
    ts_end=46_000_000,
    buf="DrawFrames 22",
    tid=SYSUI_RTID,
    pid=SYSUI_PID)
trace.add_atrace_for_thread(
    ts=58_000_000,
    ts_end=60_000_000,
    buf="DrawFrames 40",
    tid=SYSUI_RTID,
    pid=SYSUI_PID)

# --- CPU 2: Two hwuiTask* threads running during DrawFrames 20 ---
trace.add_ftrace_packet(cpu=2)
trace.add_sched(
    ts=28_500_000, prev_pid=0, next_pid=HWUI_TID_0, next_comm="hwuiTask0")
trace.add_sched(
    ts=29_000_000,
    prev_pid=HWUI_TID_0,
    next_pid=HWUI_TID_1,
    prev_comm="hwuiTask0",
    prev_state="S",
    next_comm="hwuiTask1")
trace.add_sched(
    ts=29_500_000,
    prev_pid=HWUI_TID_1,
    next_pid=0,
    prev_comm="hwuiTask1",
    prev_state="S")

# --- FrameTimeline packets ---
add_frame_timeline(trace, ts=26_000_000, dur=7_000_000, token=20, pid=SYSUI_PID)
add_frame_timeline(trace, ts=43_000_000, dur=7_000_000, token=22, pid=SYSUI_PID)
add_frame_timeline(trace, ts=56_000_000, dur=7_000_000, token=40, pid=SYSUI_PID)

# --- Linux Perf HW_INSTRUCTIONS counter samples ---
add_perf_sample_defaults(trace)
# CPU 0: pre-CUJ spike [5ms, 20ms] (99,000), straddling [20ms, 70ms]
# (1,000 instr/ms -> delta 50,000), post-CUJ spike [70ms, 90ms] (99,000).
add_perf_instruction_sample(
    trace, ts=5_000_000, cpu=0, pid=SYSUI_PID, tid=SYSUI_PID, instructions=0)
add_perf_instruction_sample(
    trace,
    ts=20_000_000,
    cpu=0,
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    instructions=99_000)
add_perf_instruction_sample(
    trace,
    ts=70_000_000,
    cpu=0,
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    instructions=149_000)
add_perf_instruction_sample(
    trace,
    ts=90_000_000,
    cpu=0,
    pid=SYSUI_PID,
    tid=SYSUI_PID,
    instructions=248_000)

# CPU 1: 2,000 instr/ms over [20ms, 70ms] (delta 100,000).
add_perf_instruction_sample(
    trace, ts=20_000_000, cpu=1, pid=SYSUI_PID, tid=SYSUI_RTID, instructions=0)
add_perf_instruction_sample(
    trace,
    ts=70_000_000,
    cpu=1,
    pid=SYSUI_PID,
    tid=SYSUI_RTID,
    instructions=100_000)

# CPU 2: 500 instr/ms over [20ms, 50ms] (delta 15,000).
add_perf_instruction_sample(
    trace, ts=20_000_000, cpu=2, pid=SYSUI_PID, tid=HWUI_TID_0, instructions=0)
add_perf_instruction_sample(
    trace,
    ts=50_000_000,
    cpu=2,
    pid=SYSUI_PID,
    tid=HWUI_TID_0,
    instructions=15_000)

sys.stdout.buffer.write(trace.trace.SerializeToString())
