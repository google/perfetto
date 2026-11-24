# Copyright (C) 2026 The Android Open Source Project
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

import abc
from typing import List
from typing import Optional
from typing import Union

from perfetto.trace_processor.protos import ProtoFactory


class TraceProcessorBackend(abc.ABC):
  """Interface that the TraceProcessor client API uses to communicate with
  different trace processor backends.
  """

  def __init__(self, protos: ProtoFactory):
    self.protos = protos

  @abc.abstractmethod
  def execute_query(self, query: str):
    ...

  @abc.abstractmethod
  def compute_metric(self, metrics: List[str]):
    ...

  @abc.abstractmethod
  def trace_summary(self,
                    specs: List[Union[str, bytes]],
                    metric_ids: Optional[List[str]] = None,
                    metadata_query_id: Optional[str] = None):
    ...

  @abc.abstractmethod
  def enable_metatrace(self):
    ...

  @abc.abstractmethod
  def disable_and_read_metatrace(self):
    ...

  @abc.abstractmethod
  def export(self, output_file, export_format: str):
    ...

  @abc.abstractmethod
  def parse(self, chunk: bytes):
    ...

  @abc.abstractmethod
  def notify_eof(self):
    ...

  @abc.abstractmethod
  def close(self):
    ...

  def _fill_trace_summary_args(self,
                               out_args,
                               specs: List[Union[str, bytes]],
                               metric_ids: Optional[List[str]] = None,
                               metadata_query_id: Optional[str] = None):
    """Fills `out_args`, a TraceSummaryArgs, from trace_summary() arguments."""
    if (metric_ids is None):
      out_args.computation_spec.run_all_metrics = True
    elif (len(metric_ids) > 0):
      out_args.computation_spec.metric_ids.extend(metric_ids)

    if (specs is not None and len(specs) > 0):
      for spec in specs:
        if isinstance(spec, str):
          out_args.textproto_specs.append(spec)
        elif isinstance(spec, bytes):
          proto_spec = self.protos.TraceSummarySpec()
          proto_spec.ParseFromString(spec)
          out_args.proto_specs.append(proto_spec)

    if (metadata_query_id is not None):
      out_args.computation_spec.metadata_query_id = metadata_query_id

    out_args.output_format = self.protos.TraceSummaryArgs.Format.BINARY_PROTOBUF
