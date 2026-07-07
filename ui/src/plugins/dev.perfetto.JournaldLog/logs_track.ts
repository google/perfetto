// Copyright (C) 2026 The Android Open Source Project
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
import {Time} from '../../base/time';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../../components/widgets/timestamp';
import {Spinner} from '../../widgets/spinner';

// Journald priority: 0=EMERG … 7=DEBUG (lower number = increased severity).
const DEPTH_TO_COLOR = [
  makeColorScheme(new HSLColor({h: 268, s: 100, l: 65})), // 0 EMERG - magenta
  makeColorScheme(new HSLColor({h: 8, s: 89, l: 56})), // 1 ALERT
  makeColorScheme(new HSLColor({h: 22, s: 88, l: 54})), // 2 CRIT
  makeColorScheme(new HSLColor({h: 0, s: 86, l: 60})), // 3 ERROR - red
  makeColorScheme(new HSLColor({h: 38, s: 100, l: 58})), // 4 WARNING - amber
  makeColorScheme(new HSLColor({h: 95, s: 52, l: 45})), // 5 NOTICE - green
  makeColorScheme(new HSLColor({h: 217, s: 100, l: 60})), // 6 INFO - blue
  makeColorScheme(new HSLColor({h: 0, s: 0, l: 60})), // 7 DEBUG - grey
];

const EVT_PX = 6;

const LOGS_SQL = `
  select
    id,
    ts,
    prio,
    utid,
    tag,
    msg,
    CASE
      WHEN prio >= 0 AND prio <= 7 THEN prio
      ELSE 7
    END as depth
  from linux_systemd_journald_logs
  order by ts
  -- linux_systemd_journald_logs aren't guaranteed to be ordered by ts, but this is a
  -- requirement for SliceTrack's mipmap operator to work
  -- correctly, so we must explicitly sort them above.
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

export function createJournaldLogTrack(trace: Trace, uri: string) {
  return SliceTrack.create({
    trace,
    uri,
    rootTableName: 'linux_systemd_journald_logs',
    dataset: new SourceDataset({
      src: LOGS_SQL,
      schema: LOGS_SCHEMA,
    }),
    initialMaxDepth: 7,
    colorizer: (row) => DEPTH_TO_COLOR[row.depth],
    tooltip: (slice) => [m('', m('b', slice.row.tag)), m('', slice.row.msg)],
    instantStyle: {
      width: EVT_PX,
      render: (ctx, r) => ctx.fillRect(r.x, r.y, r.width, r.height),
    },
    sliceLayout: {padding: 2, sliceHeight: 7},
    detailsPanel: (row) => {
      let msg: string | undefined;

      trace.engine
        .query(
          `select msg from linux_systemd_journald_logs where id = ${row.id}`,
        )
        .then((result) => {
          msg = result.maybeFirstRow({msg: STR})?.msg;
        });

      return {
        render() {
          return m(
            DetailsShell,
            {title: 'Journald Log'},
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
                    m(TreeNode, {left: 'Priority', right: row.prio}),
                    m(TreeNode, {left: 'Tag', right: row.tag}),
                    m(TreeNode, {left: 'Utid', right: row.utid}),
                    m(TreeNode, {
                      left: 'Message',
                      right: msg ? msg : m(Spinner),
                    }),
                  ),
                ),
              ),
            ),
          );
        },
      };
    },
  });
}
