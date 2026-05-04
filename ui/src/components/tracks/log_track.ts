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
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {SliceTrack} from './slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {ColorScheme} from '../../base/color_scheme';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Timestamp} from '../widgets/timestamp';
import {Time} from '../../base/time';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Spinner} from '../../widgets/spinner';

const EVT_PX = 6; // Width of an event tick in pixels.

export interface LogTrackConfig {
  /** Track / details panel title, e.g. "Android Log" or "Journald Log". */
  readonly title: string;
  /**
   * The SQL fragment that provides rows with columns:
   *   id, ts, prio, utid, tag, msg, depth
   * It should already be wrapped in `select ... from ... order by ts`.
   */
  readonly sqlSource: string;
  /** Root table name forwarded to SliceTrack (e.g. 'logs', 'journald_logs'). */
  readonly rootTableName: string;
  /**
   * Color scheme per depth level; index 0 = depth 0 (most severe in context).
   */
  readonly depthColors: ColorScheme[];
  /** Async callback to fetch the log message for the details panel. */
  readonly fetchMsg: (trace: Trace, id: number) => Promise<string | undefined>;
}

/**
 * Creates a SliceTrack for log events given a LogTrackConfig.
 * Both the AndroidLog and JournaldLog plugins use this factory.
 */
export function createLogTrack(
  trace: Trace,
  uri: string,
  config: LogTrackConfig,
) {
  return SliceTrack.create({
    trace,
    uri,
    rootTableName: config.rootTableName,
    dataset: new SourceDataset({
      src: config.sqlSource,
      schema: {
        id: NUM,
        ts: LONG,
        prio: NUM,
        utid: NUM_NULL,
        depth: NUM,
        tag: STR_NULL,
        msg: STR_NULL,
      },
    }),
    initialMaxDepth: 4,
    colorizer: (row) => config.depthColors[row.depth],
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
      // The msg is initially undefined; it'll be filled in when it loads.
      let msg: string | undefined;

      config.fetchMsg(trace, row.id).then((result) => {
        msg = result;
      });

      return {
        render() {
          return m(
            DetailsShell,
            {title: config.title},
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
