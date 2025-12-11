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

const DEPTH_TO_COLOR = [
  makeColorScheme(new HSLColor({h: 122, s: 39, l: 49})),
  makeColorScheme(new HSLColor({h: 0, s: 0, l: 70})),
  makeColorScheme(new HSLColor({h: 45, s: 100, l: 51})),
  makeColorScheme(new HSLColor({h: 4, s: 90, l: 58})),
  makeColorScheme(new HSLColor({h: 291, s: 64, l: 42})),
];

const EVT_PX = 6; // Width of an event tick in pixels.

export function createAndroidLogTrack(trace: Trace, uri: string) {
  return SliceTrack.create({
    trace,
    uri,
    rootTableName: 'android_logs',
    dataset: new SourceDataset({
      src: `
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
        -- android_logs aren't guaranteed to be ordered by ts, but this is a
        -- requirements for SliceTrack's mipmap operator to work 
        -- correctly, so we must explicitly sort them above.
      `,
      schema: {
        id: NUM,
        ts: LONG,
        prio: NUM,
        utid: NUM,
        depth: NUM,
        tag: STR_NULL,
        msg: STR_NULL,
      },
    }),
    initialMaxDepth: 4,
    colorizer: (row) => DEPTH_TO_COLOR[row.depth],
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
    detailsPanel: (row) => {
      // The msg is initially undefined, it'll be filled in when it loads
      let msg: string | undefined;

      // Quickly load the log message
      trace.engine
        .query(`select msg from android_logs where id = ${row.id}`)
        .then((result) => {
          const resultRow = result.maybeFirstRow({msg: STR});
          msg = resultRow?.msg;
        });

      return {
        render() {
          return m(
            DetailsShell,
            {
              title: `Android Log`,
            },
            m(
              GridLayout,
              m(
                GridLayoutColumn,
                m(
                  Section,
                  {title: 'Details'},
                  m(
                    Tree,
                    m(TreeNode, {
                      left: 'ID',
                      right: row.id,
                    }),
                    m(TreeNode, {
                      left: 'Timestamp',
                      right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
                    }),
                    m(TreeNode, {
                      left: 'Priority',
                      right: row.prio,
                    }),
                    m(TreeNode, {
                      left: 'Tag',
                      right: row.tag,
                    }),
                    m(TreeNode, {
                      left: 'Utid',
                      right: row.utid,
                    }),
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
