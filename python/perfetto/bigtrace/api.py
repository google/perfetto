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

class Bigtrace:

  def __init__(self):
    channel = grpc.insecure_channel("localhost:5051")
    self.stub = BigtraceOrchestratorStub(channel)

  def query(self, traces: List[str], sql_query: str):
    if not traces:
      raise PerfettoException("Trace list cannot be empty")
    if not sql_query:
      raise PerfettoException("SQL query cannot be empty")
    # Query and then convert to pandas
    tables = []
    args = BigtraceQueryArgs(traces=traces, sql_query=sql_query)

    responses = self.stub.Query(args)
    try:
      for response in responses:
        repeated_batches = []
        results = response.result
        column_names = results[0].column_names
        for result in results:
          repeated_batches.extend(result.batch)
        iterator = QueryResultIterator(column_names, repeated_batches)
        df = iterator.as_pandas_dataframe()
        # TODO(ivankc) Investigate whether this is the
        # best place to insert these addresses for performance
        df.insert(0, '_trace_address', response.trace)
        tables.append(df)
      flattened = pd.concat(tables)
      return flattened.reset_index(drop=True)
    except grpc.RpcError as e:
      raise PerfettoException(f"gRPC {e.code().name} error - {e.details()}")
