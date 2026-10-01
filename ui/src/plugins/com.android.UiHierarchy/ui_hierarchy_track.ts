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
import {Time} from '../../base/time';
import {Timestamp} from '../../components/widgets/timestamp';
import {materialColorScheme} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Icon} from '../../widgets/icon';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  queryScreenText,
  type ScreenTextEntry,
  type UiHierarchySnapshot,
  type UiHierarchyWindow,
} from './ui_hierarchy_data';
import {UI_HIERARCHY_ROUTE} from './ui_hierarchy_route';
import type {UiHierarchySession} from './ui_hierarchy_session';

class UiHierarchySnapshotPanel implements TrackEventDetailsPanel {
  private ts?: bigint;
  private windowTitle = 'Window';
  private snapshot?: UiHierarchySnapshot;
  private index = 0;
  private total = 0;
  private screenTexts: ScreenTextEntry[] = [];

  constructor(
    private readonly trace: Trace,
    private readonly session: UiHierarchySession,
    private readonly windowKey: string,
  ) {}

  async load(sel: TrackEventSelection): Promise<void> {
    this.ts = sel.ts;
    if (this.session.selectedWindowKey !== this.windowKey) {
      await this.session.setWindow(this.windowKey);
    }
    await this.session.setNearestTs(sel.ts);

    const win = this.session.selectedWindow;
    this.windowTitle = win
      ? `${win.title} (${win.processName ?? 'pid ' + win.pid})`
      : 'Window';
    this.index = this.session.index;
    this.total = this.session.snapshots.length;
    this.snapshot = this.session.currentSnapshot;

    this.screenTexts = await queryScreenText(
      this.trace.engine,
      sel.ts,
      win?.upid,
    );
    m.redraw();
  }

  render(): m.Children {
    const snap = this.snapshot;
    return m(
      DetailsShell,
      {
        title: 'UI Hierarchy Snapshot',
        description: `${this.windowTitle} — Snapshot #${this.index + 1} of ${this.total}`,
        buttons: m(
          'a',
          {
            href: `#!${UI_HIERARCHY_ROUTE}/${encodeURIComponent(this.windowKey)}`,
            title: 'Open full 3-pane viewer at this snapshot',
            onclick: (e: MouseEvent) => {
              e.preventDefault();
              void (async () => {
                if (this.session.selectedWindowKey !== this.windowKey) {
                  await this.session.setWindow(this.windowKey);
                }
                if (this.ts !== undefined) {
                  await this.session.setNearestTs(this.ts);
                }
                this.trace.navigate(
                  `#!${UI_HIERARCHY_ROUTE}/${encodeURIComponent(this.windowKey)}`,
                );
              })();
            },
          },
          m(Button, {
            label: 'Open in UI Hierarchy viewer',
            icon: 'open_in_full',
            intent: Intent.Primary,
          }),
        ),
      },
      m(GridLayout, [
        m(
          Section,
          {title: 'Snapshot Details'},
          m(Tree, [
            m(TreeNode, {
              left: 'Window',
              right: this.windowTitle,
            }),
            m(TreeNode, {
              left: 'Snapshot Index',
              right: `${this.index + 1} / ${this.total}`,
            }),
            this.ts !== undefined &&
              m(TreeNode, {
                left: 'Timestamp',
                right: m(Timestamp, {
                  trace: this.trace,
                  ts: Time.fromRaw(this.ts),
                }),
              }),
            snap !== undefined &&
              m(TreeNode, {
                left: 'Type',
                right: snap.isKeyframe
                  ? 'KEYFRAME (Full)'
                  : 'DELTA (Incremental)',
              }),
            snap !== undefined &&
              m(TreeNode, {
                left: 'Nodes Changed / Added',
                right: `${snap.changedNodeCount}`,
              }),
            snap !== undefined &&
              m(TreeNode, {
                left: 'Nodes Removed',
                right: `${snap.removedNodeCount}`,
              }),
            snap?.vsyncId !== undefined &&
              m(TreeNode, {
                left: 'Vsync ID',
                right: `${snap.vsyncId}`,
              }),
          ]),
        ),
        m(
          Section,
          {
            title: `Screen Text & Semantics (${this.screenTexts.length} visible)`,
          },
          this.screenTexts.length === 0
            ? m(
                '.pf-uih-muted',
                'No semantic text/labels captured at this timestamp.',
              )
            : m('table.pf-uih-props-table', [
                m('thead', [
                  m('tr', [
                    m('th', 'Role'),
                    m('th', 'Label / Text'),
                    m('th', 'Tag'),
                    m('th', 'Clickable'),
                  ]),
                ]),
                m('tbody', [
                  this.screenTexts.map((st) =>
                    m('tr', [
                      m('td.pf-uih-code', st.role),
                      m('td', `"${st.label}"`),
                      m('td', st.testTag ? `#${st.testTag}` : '-'),
                      m('td', st.isClickable ? m(Icon, {icon: 'check'}) : ''),
                    ]),
                  ),
                ]),
              ]),
        ),
      ]),
    );
  }
}

export function createUiHierarchyTrack(
  trace: Trace,
  uri: string,
  window: UiHierarchyWindow,
  session: UiHierarchySession,
) {
  // Snapshot packets are per process and may cover several windows, so the
  // per-window frame table is the source of truth for this track. Each slice
  // spans one version of the window tree: from its frame until the window's
  // next frame (or removal, or the end of the trace).
  const src = `
    WITH frames AS (
      SELECT
        f.id,
        f.ts,
        f.is_removed,
        s.is_keyframe,
        LEAD(f.ts, 1, trace_end()) OVER (ORDER BY f.ts) - f.ts AS dur
      FROM __intrinsic_ui_hierarchy_window_frame f
      JOIN __intrinsic_ui_hierarchy_snapshot s ON s.id = f.snapshot_id
      WHERE f.upid = ${window.upid} AND f.window_id = ${window.windowId}
    ),
    changed AS (
      SELECT ts, COUNT(*) AS cnt
      FROM __intrinsic_ui_hierarchy_node
      WHERE upid = ${window.upid} AND window_id = ${window.windowId}
      GROUP BY ts
    )
    SELECT
      id,
      ts,
      dur,
      0 AS depth,
      IIF(is_keyframe, 'Keyframe', 'Update')
        || IIF(IFNULL(cnt, 0) > 0, ' (' || cnt || ' changed)', '') AS name
    FROM frames
    LEFT JOIN changed USING (ts)
    WHERE NOT is_removed
  `;
  const panel = new UiHierarchySnapshotPanel(trace, session, window.key);
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      schema: {id: NUM, ts: LONG, dur: LONG, name: STR, depth: NUM},
      src,
    }),
    colorizer: (row) =>
      materialColorScheme(
        row.name.startsWith('Keyframe') ? 'keyframe_accent' : 'delta_node',
      ),
    detailsPanel: () => panel,
  });
}
