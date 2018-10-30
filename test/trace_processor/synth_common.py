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

import argparse

from google.protobuf import descriptor, descriptor_pb2, message_factory, reflection
from google.protobuf.pyext import _message


class Trace(object):
    def __init__(self, trace):
        self.trace = trace
        self.proc_map = {}
        self.proc_map[0] = "idle_thread"

    def add_ftrace_packet(self, cpu):
        self.packet = self.trace.packet.add()
        self.packet.ftrace_events.cpu = cpu

    def add_sched(self, ts, prev_pid, next_pid):
        ftrace = self.packet.ftrace_events.event.add()
        ftrace.timestamp = ts

        ss = ftrace.sched_switch
        ss.prev_comm = self.proc_map[prev_pid]
        ss.prev_pid = prev_pid
        ss.next_pid = next_pid
        ss.next_comm = self.proc_map[next_pid]

    def add_cpufreq(self, ts, freq, cpu):
        ftrace = self.packet.ftrace_events.event.add()
        ftrace.timestamp = ts

        cpufreq = ftrace.cpu_frequency
        cpufreq.state = freq
        cpufreq.cpu_id = cpu

    def add_process_tree_packet(self):
        self.packet = self.trace.packet.add()

    def add_process(self, pid, ppid, cmdline):
        process = self.packet.process_tree.processes.add()
        process.pid = pid
        process.ppid = ppid
        process.cmdline.append(cmdline)

        self.proc_map[pid] = cmdline

    def add_thread(self, tid, tgid, cmdline):
        thread = self.packet.process_tree.threads.add()
        thread.tid = tid
        thread.tgid = tgid

        self.proc_map[tid] = cmdline


def create_trace():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        'trace_descriptor', type=str, help='location of trace descriptor')
    args = parser.parse_args()

    with open(args.trace_descriptor, "rb") as t:
        fileContent = t.read()

    file_desc_set_pb2 = descriptor_pb2.FileDescriptorSet()
    file_desc_set_pb2.MergeFromString(fileContent)

    desc_by_path = {}
    for f_desc_pb2 in file_desc_set_pb2.file:
        f_desc_pb2_encode = f_desc_pb2.SerializeToString()
        f_desc = descriptor.FileDescriptor(
            name=f_desc_pb2.name,
            package=f_desc_pb2.package,
            serialized_pb=f_desc_pb2_encode)

        for desc in f_desc.message_types_by_name.values():
            desc_by_path[desc.full_name] = desc

    trace = message_factory.MessageFactory().GetPrototype(
        desc_by_path["perfetto.protos.Trace"])()
    return Trace(trace)
