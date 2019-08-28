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
trace.add_buffer_event_packet(ts=1, buffer_id=1, layer_name="layerName1", frame_number=1, event_type=1, duration=6)
trace.add_buffer_event_packet(ts=2, buffer_id=2, layer_name="layerName2", frame_number=2, event_type=2, duration=7)
trace.add_buffer_event_packet(ts=3, buffer_id=3, layer_name="layerName3", frame_number=3, event_type=3, duration=8)
trace.add_buffer_event_packet(ts=4, buffer_id=4, layer_name="layerName4", frame_number=4, event_type=4, duration=9)
trace.add_buffer_event_packet(ts=5, buffer_id=5, layer_name="layerName5", frame_number=5, event_type=5, duration=10)
# Missing id.
trace.add_buffer_event_packet(ts=6, buffer_id=-1, layer_name="layerName6", frame_number=6, event_type=6, duration=11)
# Missing type.
trace.add_buffer_event_packet(ts=7, buffer_id=7, layer_name="layerName7", frame_number=7, event_type=-1, duration=12)

print(trace.trace.SerializeToString())
