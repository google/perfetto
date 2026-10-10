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


class TraceProcessorRemote(abc.ABC):
  """Interface for a connection to a running trace processor instance.

  TraceProcessor talks to trace processor only through this interface, so it
  doesn't need to know which protocol or transport a connection uses.

  Currently implemented by TraceProcessorHttp (HTTP server, also used for the
  trace processor subprocess TraceProcessor starts itself) and
  TraceProcessorRpc (RPC protocol over a byte transport, e.g.
  TraceProcessorUnix for `trace_processor server unix` sessions). See
  remote_factory.create_remote() to connect to a running instance.
  """

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
