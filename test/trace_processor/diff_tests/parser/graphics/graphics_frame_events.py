#!/usr/bin/env python3
# Copyright (C) 2019 The Android Open Source Project
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


class BufferEvent:
  UNSPECIFIED = 0
  DEQUEUE = 1
  QUEUE = 2
  POST = 3
  ACQUIRE_FENCE = 4
  LATCH = 5
  HWC_COMPOSITION_QUEUED = 6
  FALLBACK_COMPOSITION = 7
  PRESENT_FENCE = 8
  RELEASE_FENCE = 9
  MODIFY = 10
  DETACH = 11
  ATTACH = 12
  CANCEL = 13


trace = synth_common.create_trace()
# Layer 1
trace.add_buffer_event_packet(
    ts=1,
    buffer_id=1,
    layer_name="layer1",
    frame_number=11,
    event_type=BufferEvent.DEQUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=4,
    buffer_id=1,
    layer_name="layer1",
    frame_number=11,
    event_type=BufferEvent.QUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=6,
    buffer_id=1,
    layer_name="layer1",
    frame_number=11,
    event_type=BufferEvent.ACQUIRE_FENCE,
    duration=0)
trace.add_buffer_event_packet(
    ts=8,
    buffer_id=1,
    layer_name="layer1",
    frame_number=11,
    event_type=BufferEvent.LATCH,
    duration=0)
trace.add_buffer_event_packet(
    ts=14,
    buffer_id=1,
    layer_name="layer1",
    frame_number=11,
    event_type=BufferEvent.PRESENT_FENCE,
    duration=0)
# Layer 2
trace.add_buffer_event_packet(
    ts=6,
    buffer_id=2,
    layer_name="layer2",
    frame_number=12,
    event_type=BufferEvent.DEQUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=8,
    buffer_id=2,
    layer_name="layer2",
    frame_number=12,
    event_type=BufferEvent.ACQUIRE_FENCE,
    duration=0)
trace.add_buffer_event_packet(
    ts=9,
    buffer_id=2,
    layer_name="layer2",
    frame_number=12,
    event_type=BufferEvent.QUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=11,
    buffer_id=2,
    layer_name="layer2",
    frame_number=12,
    event_type=BufferEvent.LATCH,
    duration=0)
trace.add_buffer_event_packet(
    ts=16,
    buffer_id=2,
    layer_name="layer2",
    frame_number=12,
    event_type=BufferEvent.PRESENT_FENCE,
    duration=0)
# Next Present of layer 1
trace.add_buffer_event_packet(
    ts=24,
    buffer_id=1,
    layer_name="layer1",
    frame_number=13,
    event_type=BufferEvent.PRESENT_FENCE,
    duration=0)
# Missing id.
trace.add_buffer_event_packet(
    ts=6,
    buffer_id=-1,
    layer_name="layer6",
    frame_number=14,
    event_type=BufferEvent.HWC_COMPOSITION_QUEUED,
    duration=0)
# Missing type.
trace.add_buffer_event_packet(
    ts=7,
    buffer_id=7,
    layer_name="layer7",
    frame_number=15,
    event_type=-1,
    duration=0)
# Missing Acquire
trace.add_buffer_event_packet(
    ts=31,
    buffer_id=1,
    layer_name="layer1",
    frame_number=21,
    event_type=BufferEvent.DEQUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=34,
    buffer_id=1,
    layer_name="layer1",
    frame_number=21,
    event_type=BufferEvent.QUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=37,
    buffer_id=1,
    layer_name="layer1",
    frame_number=22,
    event_type=BufferEvent.DEQUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=41,
    buffer_id=1,
    layer_name="layer1",
    frame_number=22,
    event_type=BufferEvent.QUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=46,
    buffer_id=1,
    layer_name="layer1",
    frame_number=22,
    event_type=BufferEvent.ACQUIRE_FENCE,
    duration=0)
# Missing queue with acquire
trace.add_buffer_event_packet(
    ts=53,
    buffer_id=2,
    layer_name="layer2",
    frame_number=24,
    event_type=BufferEvent.DEQUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=59,
    buffer_id=2,
    layer_name="layer2",
    frame_number=24,
    event_type=BufferEvent.ACQUIRE_FENCE,
    duration=0)
trace.add_buffer_event_packet(
    ts=61,
    buffer_id=2,
    layer_name="layer2",
    frame_number=24,
    event_type=BufferEvent.LATCH,
    duration=0)
# Missing queue without acquire
trace.add_buffer_event_packet(
    ts=63,
    buffer_id=1,
    layer_name="layer1",
    frame_number=25,
    event_type=BufferEvent.DEQUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=73,
    buffer_id=1,
    layer_name="layer1",
    frame_number=26,
    event_type=BufferEvent.DEQUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=75,
    buffer_id=1,
    layer_name="layer1",
    frame_number=26,
    event_type=BufferEvent.QUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=79,
    buffer_id=1,
    layer_name="layer1",
    frame_number=26,
    event_type=BufferEvent.ACQUIRE_FENCE,
    duration=0)
# Same buffer in multiple layers
trace.add_buffer_event_packet(
    ts=81,
    buffer_id=1,
    layer_name="layer1",
    frame_number=30,
    event_type=BufferEvent.DEQUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=83,
    buffer_id=1,
    layer_name="layer1",
    frame_number=30,
    event_type=BufferEvent.QUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=90,
    buffer_id=1,
    layer_name="layer2",
    frame_number=35,
    event_type=BufferEvent.DEQUEUE,
    duration=0)
trace.add_buffer_event_packet(
    ts=92,
    buffer_id=1,
    layer_name="layer2",
    frame_number=35,
    event_type=BufferEvent.QUEUE,
    duration=0)
sys.stdout.buffer.write(trace.trace.SerializeToString())
