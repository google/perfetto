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

from google.protobuf import descriptor, descriptor_pb2, message_factory, descriptor_pool
from google.protobuf import reflection, text_format


ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def create_message_factory(descriptor_file_paths, proto_type):
  pool = descriptor_pool.DescriptorPool()
  for file_path in descriptor_file_paths:
    descriptor = read_descriptor(file_path)
    for file in descriptor.file:
      pool.Add(file)

  return message_factory.MessageFactory().GetPrototype(
      pool.FindMessageTypeByName(proto_type))


def read_descriptor(file_name):
  with open(file_name, 'rb') as f:
    contents = f.read()

  descriptor = descriptor_pb2.FileDescriptorSet()
  descriptor.MergeFromString(contents)

  return descriptor


def serialize_textproto_trace(trace_descriptor_path, extension_descriptor_paths,
                              text_proto_path, out_stream):
  proto = create_message_factory([trace_descriptor_path] +
                                 extension_descriptor_paths,
                                 'perfetto.protos.Trace')()

  with open(text_proto_path, 'r') as text_proto_file:
    text_format.Merge(text_proto_file.read(), proto)
  out_stream.write(proto.SerializeToString())
  out_stream.flush()


def serialize_python_trace(trace_descriptor_path, python_trace_path,
                           out_stream):
  python_cmd = [
      'python3',
      python_trace_path,
      trace_descriptor_path,
  ]

  # Add the test dir to the PYTHONPATH to allow synth_common to be found.
  env = os.environ.copy()
  if 'PYTHONPATH' in env:
    env['PYTHONPATH'] = "{}:{}".format(
        os.path.join(ROOT_DIR, 'test'), env['PYTHONPATH'])
  else:
    env['PYTHONPATH'] = os.path.join(ROOT_DIR, 'test')
  subprocess.check_call(python_cmd, env=env, stdout=out_stream)
