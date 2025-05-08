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
    "Lock contention on thread suspend count lock (owner tid: 0)",
    "Lock contention on a monitor lock (owner tid: 0)",
    "monitor contention with owner BG Thread #1 (30) at",
    "SuspendThreadByThreadId suspended Primes-1 id=19",
    "LoadApkAssetsFd({ParcelFileDescriptor: java.io.FileDescriptor@340019d})",
    "relayoutWindow#first=false/resize=false/vis=true/params=true/force=false",
    "android.os.Handler: kotlinx.coroutines.CancellableContinuationImpl",
    "Choreographer#doFrame 122932914", "DrawFrames 122921845",
    "/data/app/.../base.apk", "OpenDexFilesFromOat(/data/app/.../base.apk)",
    "Open oat file /data/misc/apexdata/com.android.art/dalvik-cache/boot.oat",
    "android.os.Handler: kotlinx.coroutines.internal.DispatchedContinuation",
    "GC: Wait For Completion Alloc",
    'android.view.ViewRootImpl$ViewRootHandler: android.view.View$$Lambda4',
    'android.os.AsyncTask$InternalHandler: #1',
    'android.os.Handler: com.android.systemui.broadcast.ActionReceiver$1$1',
    'com.android.keyguard.KeyguardUpdateMonitor$13: #302',
    'android.os.Handler: com.android.systemui.qs.TileServiceManager$1',
    'FrameBuffer-201#invokeListeners-non-direct',
    'Transaction (ptz-fgd-1-LOCAL_MEDIA_REMOVE_DELETED_ITEMS_SYNC, 11910)',
    'InputConsumer processing on ClientState{e1d234a mUid=1234 mPid=1234 '
    'mSelfReportedDisplayId=0} (0xb000000000000000)',
    'InputConsumer processing on [Gesture Monitor] swipe-up '
    '(0xb000000000000000)',
    '+job=1234:"com.google.android.apps.internal.betterbug"',
    'Looper.dispatch: android.app.ActivityThread$H(runnable@a9f7a84'
    '(android.app.ActivityThread@1d57743,40))', 'Not changed at ALL 0',
    'Three digits to replace 123 1234', 'kworker/1d57743', '1234',
    '1019b5c SurfaceView[com.google.android.apps.maps/com.google.android.maps.'
    'MapsActivity]#1(BLAST Consumer)1', '1 2 3 4', '0x1019b5c',
    'ImageDecoder#decodeDrawable', '+state=10152:"sensor:0x101002e"',
    '[0612]< SET_SIGNAL_STRENGTH_REPORTING_CRITERIA',
    'sendMessage(inputChannel=6f38b3e PopupWindow:bb19a78, seq=0x123, '
    'type=FOCUS)', 'Over the RR duration: timestamp:12345,signalTime:12345'
    ',VSyncPeriod:12345,desiredVsyncPeriod:12345,transientDuration:1'
]

for name in slices_to_standardize:
  trace.add_atrace_async_begin(
      ts=1_000_000, tid=SYSUI_PID, pid=SYSUI_PID, buf=name)
  trace.add_atrace_async_end(
      ts=2_000_000, tid=SYSUI_PID, pid=SYSUI_PID, buf=name)

sys.stdout.buffer.write(trace.trace.SerializeToString())
