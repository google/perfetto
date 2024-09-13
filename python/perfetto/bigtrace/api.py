#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from typing import List

import grpc
import pandas as pd

from perfetto.bigtrace.protos.perfetto.bigtrace.orchestrator_pb2 import BigtraceQueryArgs
from perfetto.bigtrace.protos.perfetto.bigtrace.orchestrator_pb2_grpc import BigtraceOrchestratorStub
from perfetto.common.query_result_iterator import QueryResultIterator
from perfetto.common.exceptions import PerfettoException

# C++ INT_MAX which is the maximum gRPC message size
MAX_MESSAGE_SIZE = 2147483647


class Bigtrace:

  def __init__(self,
               orchestrator_address="127.0.0.1:5051",
               wait_for_ready_for_testing=False):
    options = [('grpc.max_receive_message_length', MAX_MESSAGE_SIZE),
               ('grpc.max_message_length', MAX_MESSAGE_SIZE)]
    channel = grpc.insecure_channel(orchestrator_address, options=options)
    self.stub = BigtraceOrchestratorStub(channel)
    self.wait_for_ready_for_testing = wait_for_ready_for_testing

  def query(self, traces: List[str], sql_query: str):
    if not traces:
      raise PerfettoException("Trace list cannot be empty")
    if not sql_query:
      raise PerfettoException("SQL query cannot be empty")
    # Query and then convert to pandas
    tables = []
    args = BigtraceQueryArgs(traces=traces, sql_query=sql_query)

    try:
      responses = self.stub.Query(
          args, wait_for_ready=self.wait_for_ready_for_testing)
      for response in responses:
        repeated_batches = []
        results = response.result
        column_names = results[0].column_names
        for result in results:
          repeated_batches.extend(result.batch)
        iterator = QueryResultIterator(column_names, repeated_batches)
        df = iterator.as_pandas_dataframe()
        # TODO(b/366409021) Investigate whether this is the
        # best place to insert these addresses for performance
        df.insert(0, '_trace_address', response.trace)
        tables.append(df)
      flattened = pd.concat(tables)
      return flattened.reset_index(drop=True)
    except grpc.RpcError as e:
      raise PerfettoException(f"gRPC {e.code().name} error - {e.details()}")
