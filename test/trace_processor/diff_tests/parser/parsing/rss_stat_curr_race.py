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

# Synthetic trace exercising the race in the rss_stat_throttled hist trigger
# (b/576695708): a process in direct reclaim of another process's mm can emit
# an event with curr=true and the other process's values. Such events should
# be attributed to the owner of the mm, not to the reclaiming process.

from os import sys

import synth_common

GAME_MM = 0xAAAA
CAMERA_MM = 0xBBBB
SWAP = 2

trace = synth_common.create_trace()

trace.add_packet(ts=1)
trace.add_process(20, 1, "game")
trace.add_process(30, 1, "camera")

trace.add_ftrace_packet(1)

# Both processes report their own mm.
trace.add_rss_stat(100, tid=20, member=SWAP, size=1000, mm_id=GAME_MM, curr=1)
trace.add_rss_stat(101, tid=30, member=SWAP, size=10, mm_id=CAMERA_MM, curr=1)

# Camera reclaims game memory: correctly reported with curr=false.
trace.add_rss_stat(102, tid=30, member=SWAP, size=1100, mm_id=GAME_MM, curr=0)

# The game reports its own swap; the camera, racing in reclaim, emits a
# duplicate with curr=true. The latter should still be attributed to the game.
trace.add_rss_stat(103, tid=20, member=SWAP, size=3800, mm_id=GAME_MM, curr=1)
trace.add_rss_stat(104, tid=30, member=SWAP, size=3800, mm_id=GAME_MM, curr=1)

# Further reclaim of the game's mm must not have been rebound to the camera.
trace.add_rss_stat(105, tid=30, member=SWAP, size=3900, mm_id=GAME_MM, curr=0)

# Camera's own mm is unaffected.
trace.add_rss_stat(106, tid=30, member=SWAP, size=12, mm_id=CAMERA_MM, curr=1)

sys.stdout.buffer.write(trace.trace.SerializeToString())
