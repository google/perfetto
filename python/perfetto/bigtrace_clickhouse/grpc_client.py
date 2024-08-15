#!/usr/bin/python3
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

# Executable script used by Clickhouse to make gRPC calls to the Orchestrator
# from a TVF

import grpc
import sys
import os

from protos.perfetto.bigtrace.orchestrator_pb2 import BigtraceQueryArgs
from protos.perfetto.bigtrace.orchestrator_pb2_grpc import BigtraceOrchestratorStub
from query_result_iterator import QueryResultIterator


def main():
  orchestrator_address = os.environ.get("BIGTRACE_ORCHESTRATOR_ADDRESS")

  for input in sys.stdin:
    # Clickhouse input is specified as tab separated
    traces, sql_query = input.rstrip("\n").split("\t")
    # Convert the string representation of list of traces given by Clickhouse into
    # a Python list
    trace_list = [x[1:-1] for x in traces[1:-1].split(',')]

    channel = grpc.insecure_channel(orchestrator_address)
    stub = BigtraceOrchestratorStub(channel)
    args = BigtraceQueryArgs(traces=trace_list, sql_query=sql_query)

    responses = stub.Query(args, wait_for_ready=False)
    for response in responses:
      repeated_batches = []
      results = response.result
      column_names = results[0].column_names
      for result in results:
        repeated_batches.extend(result.batch)
      qr_it = QueryResultIterator(column_names, repeated_batches)

      for row in qr_it:
        # Retrieve all values from columns and replace nulls with empty string
        data = [(x if x else "") for x in row.__repr__().values()]
        # Convert the list to a tab separated format for Clickhouse to ingest
        data_str = '\t'.join(data)
        print(data_str + '\n', end='')

    sys.stdout.flush()


if __name__ == "__main__":
  main()
