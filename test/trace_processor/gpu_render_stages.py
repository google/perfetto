#!/usr/bin/python
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
sys.path.append(path.dirname(path.dirname(path.abspath(__file__))))
import synth_common

trace = synth_common.create_trace()

trace.add_gpu_render_stages_stage_spec([{
    'name': 'stage 0'
}, {
    'name': 'stage 1'
}, {
    'name': 'stage 2'
}])

trace.add_gpu_render_stages_hw_queue_spec([{
    'name': 'queue 0'
}, {
    'name': 'queue 1'
}])

for i in range(1, 8):
  extra_data = {}
  render_target_handle = None
  if i % 4 != 0:
    extra_data['keyOnlyTest'] = None
    if i % 2 != 0:
      extra_data['stencilBPP'] = '1'
    extra_data['height'] = str(pow(i, 2))

  trace.add_gpu_render_stages(
      ts=i * 10,
      event_id=i,
      duration=5,
      hw_queue_id=i % 2,
      stage_id=i % 3,
      context=42,
      extra_data=extra_data)

# Test stage naming with render target handle.
trace.add_gpu_render_stages(
    ts=80, event_id=8, duration=5, hw_queue_id=0, stage_id=2, context=42)

trace.add_gpu_render_stages(
    ts=90,
    event_id=9,
    duration=5,
    hw_queue_id=0,
    stage_id=0,
    context=42,
    render_target_handle=0x10)

trace.add_vk_debug_marker(
    ts=91, pid=100, vk_device=1, obj=0x10, obj_name="frame_buffer")

trace.add_gpu_render_stages(
    ts=100,
    event_id=10,
    duration=5,
    hw_queue_id=0,
    stage_id=0,
    context=42,
    render_target_handle=0x10)

trace.add_vk_debug_marker(
    ts=101, pid=100, vk_device=1, obj=0x10, obj_name="renamed_buffer")

trace.add_gpu_render_stages(
    ts=110,
    event_id=11,
    duration=5,
    hw_queue_id=0,
    stage_id=0,
    context=42,
    render_target_handle=0x10)

print(trace.trace.SerializeToString())
