// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import {makeColorScheme} from '../../components/colorizer';
import {HSLColor} from '../../base/color';
import type {Trace} from '../../public/trace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {createLogTrack} from '../../components/tracks/log_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';

// Android priority depths: prio<=3 -> 0, 4 -> 1, 5 -> 2, 6 -> 3, 7 -> 4.
const DEPTH_TO_COLOR = [
  makeColorScheme(new HSLColor({h: 0, s: 0, l: 60})), // prio<=3 V/D — grey
  makeColorScheme(new HSLColor({h: 120, s: 62, l: 55})), // prio=4  I  — green
  makeColorScheme(new HSLColor({h: 38, s: 100, l: 58})), // prio=5  W  — amber
  makeColorScheme(new HSLColor({h: 0, s: 86, l: 60})), // prio=6  E  — red
  makeColorScheme(new HSLColor({h: 268, s: 100, l: 65})), // prio=7  F  — magenta
];

const LOGS_SQL = `
  select
    id,
    ts,
    prio,
    utid,
    tag,
    msg,
    CASE
      WHEN prio <= 3 THEN 0
      WHEN prio = 4 THEN 1
      WHEN prio = 5 THEN 2
      WHEN prio = 6 THEN 3
      WHEN prio = 7 THEN 4
      ELSE -1
    END as depth
  from android_logs
  order by ts
`;

const LOGS_SCHEMA = {
  id: NUM,
  ts: LONG,
  prio: NUM,
  utid: NUM_NULL,
  depth: NUM,
  tag: STR_NULL,
  msg: STR_NULL,
};

const EVT_PX = 6;

export function createAndroidLogTrack(trace: Trace, uri: string) {
  return createLogTrack(trace, uri, {
    title: 'Android Log',
    rootTableName: 'android_logs',
    sqlSource: LOGS_SQL,
    depthColors: DEPTH_TO_COLOR,
    fetchMsg: async (t, id) => {
      const result = await t.engine.query(
        `select msg from android_logs where id = ${id}`,
      );
      return result.maybeFirstRow({msg: STR})?.msg;
    },
  });
}

export function createPerProcessLogTrack(
  trace: Trace,
  uri: string,
  utids: number[],
) {
  return SliceTrack.create({
    trace,
    uri,
    rootTableName: 'android_logs',
    dataset: new SourceDataset({
      src: LOGS_SQL,
      schema: LOGS_SCHEMA,
      filter: {col: 'utid', in: utids},
    }),
    initialMaxDepth: 4,
    colorizer: (row) => DEPTH_TO_COLOR[row.depth],
    tooltip: (slice) => [m('', m('b', slice.row.tag)), m('', slice.row.msg)],
    instantStyle: {
      width: EVT_PX,
      render: (ctx, r) => ctx.fillRect(r.x, r.y, r.width, r.height),
    },
    sliceLayout: {padding: 2, sliceHeight: 7},
  });
}

export function createPerThreadLogTrack(
  trace: Trace,
  uri: string,
  utid: number,
) {
  return SliceTrack.create({
    trace,
    uri,
    rootTableName: 'android_logs',
    dataset: new SourceDataset({
      src: LOGS_SQL,
      schema: LOGS_SCHEMA,
      filter: {col: 'utid', eq: utid},
    }),
    initialMaxDepth: 4,
    colorizer: (row) => DEPTH_TO_COLOR[row.depth],
    tooltip: (slice) => [m('', m('b', slice.row.tag)), m('', slice.row.msg)],
    instantStyle: {
      width: EVT_PX,
      render: (ctx, r) => ctx.fillRect(r.x, r.y, r.width, r.height),
    },
    sliceLayout: {padding: 2, sliceHeight: 7},
  });
}
