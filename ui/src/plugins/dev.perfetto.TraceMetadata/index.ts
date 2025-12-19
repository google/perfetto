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
import {Time} from '../../base/time';
import {SliceTrack} from '../../components/tracks/slice_track';
import {Timestamp} from '../../components/widgets/timestamp';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TraceMetadata';
  static readonly dependencies = [StandardGroupsPlugin];

  async onTraceLoad(trace: Trace): Promise<void> {
    const res = await trace.engine.query(`
      select count() as cnt from (select 1 from clock_snapshot limit 1)
    `);
    const row = res.firstRow({cnt: NUM});
    if (row.cnt === 0) {
      return;
    }
    const uri = `/clock_snapshots`;
    const track = SliceTrack.create({
      trace,
      uri,
      dataset: new SourceDataset({
        src: `
          SELECT
            id,
            ts,
            'Snapshot' as name,
            clock_id,
            clock_name,
            clock_value,
            snapshot_id,
            machine_id,
            0 as dur
          FROM clock_snapshot
        `,
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          name: STR,
          clock_id: NUM,
          clock_name: STR_NULL,
          clock_value: LONG,
          snapshot_id: NUM,
          machine_id: NUM_NULL,
        },
      }),
      detailsPanel: (row) => {
        return {
          render() {
            return m(
              DetailsShell,
              {
                title: 'Clock Snapshot',
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
                        left: 'clock_id',
                        right: row.clock_id,
                      }),
                      m(TreeNode, {
                        left: 'clock_name',
                        right: row.clock_name ?? 'NULL',
                      }),
                      m(TreeNode, {
                        left: 'clock_value',
                        right: row.clock_value.toLocaleString(),
                      }),
                      m(TreeNode, {
                        left: 'snapshot_id',
                        right: row.snapshot_id,
                      }),
                      m(TreeNode, {
                        left: 'machine_id ',
                        right: row.machine_id ?? 'NULL',
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
    trace.tracks.registerTrack({
      uri,
      renderer: track,
    });
    const trackNode = new TrackNode({uri, name: 'Clock Snapshots'});
    const group = trace.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(trace.defaultWorkspace, 'SYSTEM');
    group.addChildInOrder(trackNode);
  }
}
