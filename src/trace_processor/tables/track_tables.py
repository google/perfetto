# Copyright (C) 2022 The Android Open Source Project
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
"""Contains tables for tracks."""

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc
from python.generators.trace_processor_table.public import ColumnDoc
from python.generators.trace_processor_table.public import CppSelfTableId
from python.generators.trace_processor_table.public import CppUint32
from src.trace_processor.tables.metadata_tables import THREAD_TABLE

TRACK_TABLE = Table(
    class_name="TrackTable",
    sql_name="track",
    columns=[
        C("name", CppString()),
        C("parent_id", CppOptional(CppSelfTableId())),
        C("source_arg_set_id", CppOptional(CppUint32())),
    ],
    tabledoc=TableDoc(
        doc='''
          Tracks are a fundamental concept in trace processor and represent a
          "timeline" for events of the same type and with the same context. See
          https://perfetto.dev/docs/analysis/trace-processor#tracks for a more
          detailed explanation, with examples.
        ''',
        group='Tracks',
        columns={
            'name':
                '''
                  Name of the track; can be null for some types of tracks (e.g.
                  thread tracks).
                ''',
            'parent_id':
                '''
                  The track which is the "parent" of this track. Only non-null
                  for tracks created using Perfetto's track_event API.
                ''',
            'source_arg_set_id':
                ColumnDoc(
                    doc='''
                      Args for this track which store information about "source"
                      of this track in the trace. For example: whether this
                      track orginated from atrace, Chrome tracepoints etc.
                    ''',
                    joinable='args.arg_set_id'),
        }))

# Keep this list sorted.
ALL_TABLES = [
    TRACK_TABLE,
]
