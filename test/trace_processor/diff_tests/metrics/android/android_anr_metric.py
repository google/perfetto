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

from os import sys
import synth_common

from google.protobuf import text_format

SS_PID = 1234

trace = synth_common.create_trace()

trace.add_packet()
trace.add_process(pid=SS_PID, ppid=1, cmdline="system_server", uid=10001)

# Add first ANR.
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=1000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.google.android.app1 11167#da24554c-452a-4ae1-b74a-fb898f6e0982",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=1001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId da24554c-452a-4ae1-b74a-fb898f6e0982):Test ANR subject 1",
    cnt=1)

# Add second ANR.
# Does not include PID
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=2000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.google.android.app2#8612fece-c2f1-4aeb-9d45-8e6d9d0201cf",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=2001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 8612fece-c2f1-4aeb-9d45-8e6d9d0201cf):Test ANR subject 2",
    cnt=1)

# Add third ANR.
# Does not include PID or subject
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=3000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.google.android.app3#c25916a0-a8f0-41f3-87df-319e06471a0f",
    cnt=1)

# Add foreground BROADCAST_OF_INTENT ANR
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=4000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.disney.disneyplus 23215#1eb3813d-45d3-4a9a-ab80-0ebeb88ea25a",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=4001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 1eb3813d-45d3-4a9a-ab80-0ebeb88ea25a):Broadcast of Intent { act=android.os.action.DEVICE_IDLE_MODE_CHANGED flg=0x50000010 cmp=com.disney.disneyplus/Di.a }",
    cnt=1)

# Add background BROADCAST_OF_INTENT ANR
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=5000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.disney.disneyplus 27195#50756b89-eadc-40c9-aef2-8886adb7d936",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=5001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 50756b89-eadc-40c9-aef2-8886adb7d936):Broadcast of Intent { act=android.intent.action.DATE_CHANGED flg=0x20200010 cmp=com.disney.disneyplus/Di.a }",
    cnt=1)

# Add INPUT_DISPATCHING_TIMEOUT ANR
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=6000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.android.chrome 17874#60b9d4b6-6487-4800-bd12-3f9d547482e3",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=6001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 60b9d4b6-6487-4800-bd12-3f9d547482e3):Input dispatching timed out (88f6a9 com.android.chrome/org.chromium.chrome.browser.customtabs.CustomTabActivity is not responding. Waited 5000ms for FocusEvent(hasFocus=false)).",
    cnt=1)

# Add INPUT_DISPATCHING_TIMEOUT_NO_FOCUSED_WINDOW ANR
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=7000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.microsoft.teams 10645#2d1dff06-54a3-450b-8123-0d21e67715c4",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=7001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 2d1dff06-54a3-450b-8123-0d21e67715c4):Input dispatching timed out (Application does not have a focused window).",
    cnt=1)

# Add START_FOREGROUND_SERVICE ANR
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=8000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.google.android.apps.internal.betterbug 26587#1c733cef-dee3-42a1-bff5-6ac2bb3167ae",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=8001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 1c733cef-dee3-42a1-bff5-6ac2bb3167ae):Context.startForegroundService() did not then call Service.startForeground(): ServiceRecord{76a32df u10 com.google.android.apps.internal.betterbug/.ramdumpuploader.RamdumpUploadService c:com.google.android.apps.internal.betterbug}",
    cnt=1)

# Add EXECUTING_SERVICE ANR
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=9000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.android.systemui 2342#f2eb9ced-2327-402c-bf7c-dc498fafa5cd",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=9001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId f2eb9ced-2327-402c-bf7c-dc498fafa5cd):executing service com.android.systemui/.doze.DozeService, waited 156441ms",
    cnt=1)

# Add CONTENT_PROVIDER_NOT_RESPONDING ANR
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=11000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.android.settings 15028#9361cad8-c888-4f03-bff9-9ed8c69d583b",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=11001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 9361cad8-c888-4f03-bff9-9ed8c69d583b):ContentProvider not responding",
    cnt=1)

# GPU_HANG
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=12000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.google.android.youtube.tv 16563#600c4866-02c0-4d46-a69c-21d9ac377ad0",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=12001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 600c4866-02c0-4d46-a69c-21d9ac377ad0):App requested: Buffer processing hung up due to stuck fence. Indicates GPU hang",
    cnt=1)

# JOB_SERVICE_START
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=13000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.android.chrome 18090#fd96eb0a-ccba-474b-8044-b7cd27e812c2",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=13001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId fd96eb0a-ccba-474b-8044-b7cd27e812c2):No response to onStartJob",
    cnt=1)

# JOB_SERVICE_STOP
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=14000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.android.chrome 13534#4e1f9f12-d9bf-4d6b-9e2b-1dfeaf774859",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=14001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 4e1f9f12-d9bf-4d6b-9e2b-1dfeaf774859):No response to onStopJob",
    cnt=1)

# JOB_SERVICE_BIND
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=15000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.android.vending 18264#8a83c04e-fd35-4945-9fcc-7736f4242cae",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=15001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 8a83c04e-fd35-4945-9fcc-7736f4242cae):Timed out while trying to bind",
    cnt=1)

# BIND_APPLICATION
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=16000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.android.phone 3538#df199866-4a6a-4388-b79f-2c76b6d5bb00",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=16001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId df199866-4a6a-4388-b79f-2c76b6d5bb00):Process ProcessRecord{1a270e8 3538:com.android.phone/1001} failed to complete startup",
    cnt=1)

# FOREGROUND_SHORT_SERVICE_TIMEOUT
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=17000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.google.netops.pager 28509#62abad99-bd09-44ef-bbbb-40db5c4d5539",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=17001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 62abad99-bd09-44ef-bbbb-40db5c4d5539):A foreground service of FOREGROUND_SERVICE_TYPE_SHORT_SERVICE did not stop within a timeout: ComponentInfo{com.google.netops.pager/com.google.netops.pager.NotifierService}",
    cnt=1)

# FOREGROUND_SERVICE_TIMEOUT
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=18000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.whatsapp 11456#975b36a1-8b4a-4d69-875e-2c33e140bd1c",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=18001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 975b36a1-8b4a-4d69-875e-2c33e140bd1c):A foreground service of type dataSync did not stop within a timeout: ComponentInfo{com.whatsapp/com.whatsapp.service.GcmFGService}",
    cnt=1)

# JOB_SERVICE_NOTIFICATION_NOT_PROVIDED
trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=19000,
    pid=SS_PID,
    tid=SS_PID,
    buf="ErrorId:com.android.chrome 22768#05122f25-2f5b-4650-aeeb-cf59a9d6295a",
    cnt=1)

trace.add_ftrace_packet(cpu=0)
trace.add_atrace_counter(
    ts=19001,
    tid=SS_PID,
    pid=SS_PID,
    buf="Subject(for ErrorId 05122f25-2f5b-4650-aeeb-cf59a9d6295a):required notification not provided",
    cnt=1)

sys.stdout.buffer.write(trace.trace.SerializeToString())
