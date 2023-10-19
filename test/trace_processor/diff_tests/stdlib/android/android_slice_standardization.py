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

from os import sys, path
import synth_common

SYSUI_PID = 1000

trace = synth_common.create_trace()
trace.add_packet()

trace.add_package_list(
    ts=0, name="com.android.systemui", uid=SYSUI_PID, version_code=1)
trace.add_process(
    pid=SYSUI_PID, ppid=0, cmdline="com.android.systemui", uid=SYSUI_PID)
trace.add_ftrace_packet(cpu=0)

slices_to_standardize = [
    "Lock contention on thread list lock (owner tid: 1665)",
    "monitor contention with owner BG Thread #1 (30) at",
    "SuspendThreadByThreadId suspended Primes-1 id=19",
    "LoadApkAssetsFd({ParcelFileDescriptor: java.io.FileDescriptor@340019d})",
    "relayoutWindow#first=false/resize=false/vis=true/params=true/force=false",
    "android.os.Handler: kotlinx.coroutines.CancellableContinuationImpl",
    "Choreographer#doFrame 122932914",
    "DrawFrames 122921845",
    "/data/app/.../base.apk",
    "OpenDexFilesFromOat(/data/app/.../base.apk)",
    "Open oat file /data/misc/apexdata/com.android.art/dalvik-cache/boot.oat",
]

for name in slices_to_standardize:
  trace.add_atrace_async_begin(
      ts=1_000_000, tid=SYSUI_PID, pid=SYSUI_PID, buf=name)
  trace.add_atrace_async_end(
      ts=2_000_000, tid=SYSUI_PID, pid=SYSUI_PID, buf=name)

sys.stdout.buffer.write(trace.trace.SerializeToString())
