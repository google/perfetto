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

# Executable script used by Clickhouse to create the output schema
# for the TVF

import sys
from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig


def main():
  for input in sys.stdin:
    sql_query = input.rstrip("\n")

    try:
      config = TraceProcessorConfig(
          bin_path="/var/lib/clickhouse/user_scripts/trace_processor_shell")
      tp = TraceProcessor(config=config)
      qr_it = tp.query(sql_query)
      qr_df = qr_it.as_pandas_dataframe()
      columns = ", ".join([
          f"{x} Tuple(`int64_value` Nullable(Int64),"
          "`string_value` Nullable(String),"
          "`double_value` Nullable(Float64))" for x in qr_df.columns
      ])
    except Exception as e:
      columns = str(e)

    print(columns + '\n', end='')
    sys.stdout.flush()


if __name__ == "__main__":
  main()
