# Copyright (C) 2020 The Android Open Source Project
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

from __future__ import absolute_import

import os
import subprocess
import tempfile

from google.protobuf import descriptor, descriptor_pb2, message_factory
from google.protobuf import reflection, text_format


ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def create_message_factory(descriptor_file_path, proto_type):
  with open(descriptor_file_path, 'rb') as descriptor_file:
    descriptor_content = descriptor_file.read()

  file_desc_set_pb2 = descriptor_pb2.FileDescriptorSet()
  file_desc_set_pb2.MergeFromString(descriptor_content)

  desc_by_path = {}
  for f_desc_pb2 in file_desc_set_pb2.file:
    f_desc_pb2_encode = f_desc_pb2.SerializeToString()
    f_desc = descriptor.FileDescriptor(
        name=f_desc_pb2.name,
        package=f_desc_pb2.package,
        serialized_pb=f_desc_pb2_encode)

    for desc in f_desc.message_types_by_name.values():
      desc_by_path[desc.full_name] = desc

  return message_factory.MessageFactory().GetPrototype(desc_by_path[proto_type])


def serialize_textproto_trace(trace_descriptor_path, text_proto_path,
                              out_stream):
  trace_message_factory = create_message_factory(trace_descriptor_path,
                                                 'perfetto.protos.Trace')
  proto = trace_message_factory()
  with open(text_proto_path, 'r') as text_proto_file:
    text_format.Merge(text_proto_file.read(), proto)
  out_stream.write(proto.SerializeToString())
  out_stream.flush()


def serialize_python_trace(trace_descriptor_path, python_trace_path,
                           out_stream):
  python_cmd = ['python3', python_trace_path, trace_descriptor_path]

  # Add the test dir to the PYTHONPATH to allow synth_common to be found.
  env = os.environ.copy()
  if 'PYTHONPATH' in env:
    env['PYTHONPATH'] = "{}:{}".format(
        os.path.join(ROOT_DIR, 'test'), env['PYTHONPATH'])
  else:
    env['PYTHONPATH'] = os.path.join(ROOT_DIR, 'test')
  subprocess.check_call(python_cmd, env=env, stdout=out_stream)
