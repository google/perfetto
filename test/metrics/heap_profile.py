#!/usr/bin/python
# Copyright (C) 2018 The Android Open Source Project
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

class ProfileBuilder(object):
  def __init__(self, pid, profile_packet):
    self.profile_packet = profile_packet
    self.profile_packet.mappings.add().iid = 1 # Dummy mapping
    self.process_dump = profile_packet.process_dumps.add()
    self.process_dump.pid = pid
    self.interned = {}

  def add_sample(self, callstack_id, allocs, frees):
    sample = self.process_dump.samples.add()
    sample.callstack_id = callstack_id
    sample.self_allocated = 1000 * allocs
    sample.self_freed = 1000 * frees
    sample.alloc_count = allocs
    sample.free_count = frees

  def add_callstack(self, id, frames):
    callstack = self.profile_packet.callstacks.add()
    callstack.iid = id
    fid = 1
    for frame in frames:
      callstack.frame_ids.append(fid)
      frame_proto = self.profile_packet.frames.add()
      frame_proto.iid = fid
      frame_proto.function_name_id = self._intern(frame)
      frame_proto.mapping_id = 1
      fid += 1

  def _intern(self, value):
    if value in self.interned:
      return self.interned[value]
    next_id = len(self.interned) + 1
    self.interned[value] = next_id
    s = self.profile_packet.strings.add()
    s.iid = next_id
    s.str = value
    return next_id

trace = synth_common.create_trace()
trace.add_process_tree_packet()
trace.add_process(1, 0, 'init')
trace.add_process(pid=2, ppid=1, cmdline='system_server')

pbuilder = ProfileBuilder(pid=2, profile_packet=trace.add_profile_packet(ts=10))
pbuilder.add_sample(callstack_id=1, allocs=2, frees=1)
pbuilder.add_callstack(id=1, frames=['f1', 'f2', 'f3'])

print(trace.trace.SerializeToString())
