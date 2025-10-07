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

import os
import struct
import subprocess
import tempfile
from typing import Any, IO, List, Optional

from google.protobuf import text_format

from python.generators.diff_tests.testing import (DataPath, Path,
                                                  SimpleperfProto, TextProto,
                                                  TraceInjector)
from python.generators.diff_tests.utils import ProtoManager

ROOT_DIR = os.path.dirname(
    os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))


class TraceGenerator:
  """A helper class for generating trace files."""

  def __init__(self, trace_descriptor_path: str,
               extension_descriptor_paths: List[str]):
    self.trace_descriptor_path = trace_descriptor_path
    self.extension_descriptor_paths = extension_descriptor_paths

  def serialize_textproto_trace(self, text_proto_path: str,
                                out_stream: IO[bytes]):
    proto = ProtoManager([self.trace_descriptor_path] +
                         self.extension_descriptor_paths).create_message(
                             'perfetto.protos.Trace')()

    with open(text_proto_path, 'r') as text_proto_file:
      text_format.Merge(text_proto_file.read(), proto)
    out_stream.write(proto.SerializeToString())
    out_stream.flush()

  def serialize_python_trace(self, root_dir: str, python_trace_path: str,
                             out_stream: IO[bytes]):
    python_cmd = [
        'python3',
        python_trace_path,
        self.trace_descriptor_path,
    ]
    python_cmd.extend(self.extension_descriptor_paths)

    # Add the test dir to the PYTHONPATH to allow synth_common to be found.
    env = os.environ.copy()
    if 'PYTHONPATH' in env:
      env['PYTHONPATH'] = "{}:{}".format(
          os.path.join(root_dir, 'test'), env['PYTHONPATH'])
    else:
      env['PYTHONPATH'] = os.path.join(root_dir, 'test')
    subprocess.check_call(python_cmd, env=env, stdout=out_stream)

  def serialize_simpleperf_proto_trace(self, simpleperf_trace: SimpleperfProto,
                                       out_stream: IO[bytes]):
    # Write simpleperf_proto header
    # Magic: "SIMPLEPERF" (10 bytes, null-padded)
    magic = b"SIMPLEPERF"
    out_stream.write(magic)

    # Version: LittleEndian16 = 1
    out_stream.write(struct.pack('<H', 1))

    # Get the simpleperf Record proto message type
    proto_manager = ProtoManager([self.trace_descriptor_path] +
                                 self.extension_descriptor_paths)
    record_proto_class = proto_manager.create_message(
        'perfetto.third_party.simpleperf.proto.Record')

    # Write each record
    for record_textproto in simpleperf_trace.records:
      record = record_proto_class()
      text_format.Merge(record_textproto, record)
      record_bytes = record.SerializeToString()

      # Write record size (LittleEndian32)
      out_stream.write(struct.pack('<I', len(record_bytes)))
      # Write record data
      out_stream.write(record_bytes)

    # End marker: LittleEndian32(0)
    out_stream.write(struct.pack('<I', 0))
    out_stream.flush()


def generate_trace_file(test_case: Any, trace_descriptor_path: str,
                        extension_descriptor_paths: List[str],
                        simpleperf_descriptor: str) -> Optional[Any]:
  # We can't use delete=True here. When using that on Windows, the
  # resulting file is opened in exclusive mode (in turn that's a subtle
  # side-effect of the underlying CreateFile(FILE_ATTRIBUTE_TEMPORARY))
  # and TP fails to open the passed path.
  gen_trace_file = None
  trace_generator = TraceGenerator(trace_descriptor_path,
                                   extension_descriptor_paths)
  if test_case.blueprint.is_trace_file():
    assert test_case.trace_path
    if test_case.trace_path.endswith('.py'):
      gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
      trace_generator.serialize_python_trace(ROOT_DIR, test_case.trace_path,
                                             gen_trace_file)

    elif test_case.trace_path.endswith('.textproto'):
      gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
      trace_generator.serialize_textproto_trace(test_case.trace_path,
                                                gen_trace_file)

  elif test_case.blueprint.is_trace_textproto():
    gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
    proto = ProtoManager([trace_descriptor_path] +
                         extension_descriptor_paths).create_message(
                             'perfetto.protos.Trace')()
    assert isinstance(test_case.blueprint.trace, TextProto)
    text_format.Merge(test_case.blueprint.trace.contents, proto)
    gen_trace_file.write(proto.SerializeToString())
    gen_trace_file.flush()

  elif test_case.blueprint.is_trace_simpleperf_proto():
    gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
    # Simpleperf is a separate format, so use a dedicated generator with
    # the simpleperf descriptor instead of the general Perfetto trace extensions.
    simpleperf_generator = TraceGenerator(trace_descriptor_path,
                                          [simpleperf_descriptor])
    simpleperf_generator.serialize_simpleperf_proto_trace(
        test_case.blueprint.trace, gen_trace_file)

  else:
    gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
    with open(gen_trace_file.name, 'w') as trace_file:
      if not isinstance(test_case.blueprint.trace, (Path, DataPath)):
        trace_file.write(test_case.blueprint.trace.contents)

  if test_case.blueprint.trace_modifier is not None:
    if gen_trace_file:
      # Overwrite |gen_trace_file|.
      modify_trace(trace_descriptor_path, extension_descriptor_paths,
                   gen_trace_file.name, gen_trace_file.name,
                   test_case.blueprint.trace_modifier)
    else:
      # Create |gen_trace_file| to save the modified trace.
      gen_trace_file = tempfile.NamedTemporaryFile(delete=False)
      assert test_case.trace_path
      modify_trace(trace_descriptor_path, extension_descriptor_paths,
                   test_case.trace_path, gen_trace_file.name,
                   test_case.blueprint.trace_modifier)
  return gen_trace_file


def modify_trace(trace_descriptor_path: str,
                 extension_descriptor_paths: List[str], in_trace_path: str,
                 out_trace_path: str, modifier: TraceInjector):
  """Modifies a trace file with the given modifier."""
  trace_proto = ProtoManager([trace_descriptor_path] +
                             extension_descriptor_paths).create_message(
                                 'perfetto.protos.Trace')()

  with open(in_trace_path, "rb") as f:
    # This may raise DecodeError when |in_trace_path| isn't protobuf.
    trace_proto.ParseFromString(f.read())
    # Modify the trace proto object with the provided modifier function.
    modifier.inject(trace_proto)

  with open(out_trace_path, "wb") as f:
    f.write(trace_proto.SerializeToString())
    f.flush()
