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
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {makeColorScheme} from '../../components/colorizer';
import {HSLColor} from '../../base/color';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../../components/widgets/timestamp';
import {Time} from '../../base/time';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Spinner} from '../../widgets/spinner';
import {ColorScheme} from '../../base/color_scheme';

const PRIO_TO_COLOR: Record<number, ColorScheme> = {
  2: makeColorScheme(new HSLColor({h: 0, s: 0, l: 60})), // V - grey
  3: makeColorScheme(new HSLColor({h: 217, s: 100, l: 60})), // D - blue
  4: makeColorScheme(new HSLColor({h: 120, s: 62, l: 55})), // I - green
  5: makeColorScheme(new HSLColor({h: 38, s: 100, l: 58})), // W - amber
  6: makeColorScheme(new HSLColor({h: 0, s: 86, l: 60})), // E - red
  7: makeColorScheme(new HSLColor({h: 268, s: 100, l: 65})), // F - magenta
};
const DEFAULT_PRIO_COLOR = makeColorScheme(new HSLColor({h: 0, s: 0, l: 60}));

const EVT_PX = 6; // Width of an event tick in pixels.

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
  utid: NUM,
  depth: NUM,
  tag: STR_NULL,
  msg: STR_NULL,
};

const PRIO_LABELS: Record<number, string> = {
  2: 'Verbose',
  3: 'Debug',
  4: 'Info',
  5: 'Warn',
  6: 'Error',
  7: 'Fatal',
};

function makeDetailsPanel(
  trace: Trace,
  row: {
    id: number;
    ts: bigint;
    prio: number;
    tag: string | null;
    utid: number;
  },
) {
  // The msg is initially undefined, it'll be filled in when it loads
  let msg: string | undefined;
  let threadInfo: string | undefined;

  // Quickly load the log message
  trace.engine
    .query(`select msg from android_logs where id = ${row.id}`)
    .then((result) => {
      msg = result.maybeFirstRow({msg: STR})?.msg;
    });

  trace.engine
    .query(`select tid, name from thread where utid = ${row.utid}`)
    .then((result) => {
      const r = result.maybeFirstRow({tid: NUM, name: STR_NULL});
      threadInfo = r
        ? r.name
          ? `${r.name} [${r.tid}]`
          : `${r.tid}`
        : `utid=${row.utid}`;
    });

  const prioLabel = PRIO_LABELS[row.prio] ?? `${row.prio}`;

  return {
    render() {
      return m(
        DetailsShell,
        {title: `Android Log`},
        m(
          GridLayout,
          m(
            GridLayoutColumn,
            m(
              Section,
              {title: 'Details'},
              m(
                Tree,
                m(TreeNode, {left: 'ID', right: row.id}),
                m(TreeNode, {
                  left: 'Timestamp',
                  right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
                }),
                m(TreeNode, {left: 'Priority', right: prioLabel}),
                m(TreeNode, {left: 'Tag', right: row.tag}),
                m(TreeNode, {
                  left: 'Thread',
                  right: threadInfo ?? m(Spinner),
                }),
                m(TreeNode, {
                  left: 'Message',
                  right: msg !== undefined ? msg : m(Spinner),
                }),
              ),
            ),
          ),
        ),
      );
    },
  };
}

export function createAndroidLogTrack(trace: Trace, uri: string) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({src: LOGS_SQL, schema: LOGS_SCHEMA}),
    initialMaxDepth: 4,
    colorizer: (row) => PRIO_TO_COLOR[row.prio] ?? DEFAULT_PRIO_COLOR,
    tooltip: (slice) => [m('', m('b', slice.row.tag)), m('', slice.row.msg)],
    // All log events are instant events, render them as a little box rather
    // than the default chevron.
    instantStyle: {
      width: EVT_PX,
      render: (ctx, r) => ctx.fillRect(r.x, r.y, r.width, r.height),
    },
    // Make rows a little more compact.
    sliceLayout: {
      padding: 2,
      sliceHeight: 7,
    },
    detailsPanel: (row) => makeDetailsPanel(trace, row),
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
    dataset: new SourceDataset({
      src: LOGS_SQL,
      schema: LOGS_SCHEMA,
      filter: {col: 'utid', in: utids},
    }),
    initialMaxDepth: 4,
    colorizer: (row) => PRIO_TO_COLOR[row.prio] ?? DEFAULT_PRIO_COLOR,
    tooltip: (slice) => [m('', m('b', slice.row.tag)), m('', slice.row.msg)],
    instantStyle: {
      width: EVT_PX,
      render: (ctx, r) => ctx.fillRect(r.x, r.y, r.width, r.height),
    },
    sliceLayout: {
      padding: 2,
      sliceHeight: 7,
    },
    detailsPanel: (row) => makeDetailsPanel(trace, row),
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
    colorizer: (row) => PRIO_TO_COLOR[row.prio] ?? DEFAULT_PRIO_COLOR,
    tooltip: (slice) => [m('', m('b', slice.row.tag)), m('', slice.row.msg)],
    instantStyle: {
      width: EVT_PX,
      render: (ctx, r) => ctx.fillRect(r.x, r.y, r.width, r.height),
    },
    sliceLayout: {
      padding: 2,
      sliceHeight: 7,
    },
    detailsPanel: (row) => makeDetailsPanel(trace, row),
  });
}
