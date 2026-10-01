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
import {DurationWidget} from '../../components/widgets/duration';
import {materialColorScheme} from '../../components/colorizer';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {
  queryScreenText,
  type ScreenTextEntry,
  type UiHierarchyWindow,
} from './ui_hierarchy_data';
import {UI_HIERARCHY_ROUTE} from './ui_hierarchy_route';
import type {UiHierarchySession} from './ui_hierarchy_session';

interface RecompositionCauseInfo {
  readonly scopeId?: bigint;
  readonly scopeName?: string;
  readonly stateId?: bigint;
  readonly stateType?: string;
  readonly stateValue?: string;
}

class ComposeEventDetailsPanel implements TrackEventDetailsPanel {
  private ts?: bigint;
  private dur?: bigint;
  private name = '';
  private eventType = '';
  private scopeId?: bigint;
  private stateId?: bigint;
  private value?: string;
  private dirty1?: bigint;
  private dirty2?: bigint;
  private causes: RecompositionCauseInfo[] = [];
  private matchingWindow?: UiHierarchyWindow;
  private screenTexts: ScreenTextEntry[] = [];

  constructor(
    private readonly trace: Trace,
    private readonly session?: UiHierarchySession,
  ) {}

  async load(sel: TrackEventSelection): Promise<void> {
    this.ts = sel.ts;
    this.dur = sel.dur;

    const res = await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE android.ui_hierarchy;
      SELECT
        upid,
        window_id,
        type,
        name,
        scope_id,
        state_id,
        value,
        dirty1,
        dirty2
      FROM android_ui_hierarchy_compose_event
      WHERE id = ${sel.eventId}
      LIMIT 1;
    `);
    const it = res.iter({
      upid: NUM_NULL,
      window_id: NUM_NULL,
      type: STR,
      name: STR_NULL,
      scope_id: LONG_NULL,
      state_id: LONG_NULL,
      value: STR_NULL,
      dirty1: LONG_NULL,
      dirty2: LONG_NULL,
    });
    if (it.valid()) {
      this.eventType = it.type;
      this.name = it.name ?? it.type;
      this.scopeId = it.scope_id ?? undefined;
      this.stateId = it.state_id ?? undefined;
      this.value = it.value ?? undefined;
      this.dirty1 = it.dirty1 ?? undefined;
      this.dirty2 = it.dirty2 ?? undefined;
      const upid = it.upid;
      const windowId = it.window_id;
      if (upid !== null && this.session !== undefined) {
        const matchingWindows = this.session.windows.filter(
          (w) => w.upid === upid,
        );
        if (windowId !== null) {
          this.matchingWindow = matchingWindows.find(
            (w) => w.windowId === windowId,
          );
        }
        if (!this.matchingWindow && matchingWindows.length > 0) {
          this.matchingWindow =
            matchingWindows.find(
              (w) =>
                !w.title.includes('DropTarget') &&
                !w.title.includes('Overlay') &&
                !w.title.includes('ScreenDecor'),
            ) ?? matchingWindows[0];
        }
        if (this.matchingWindow) {
          if (this.session.selectedWindowKey !== this.matchingWindow.key) {
            await this.session.setWindow(this.matchingWindow.key);
          }
          if (this.ts !== undefined) {
            await this.session.setNearestTs(this.ts);
          }
        }
      }
      if (this.ts !== undefined) {
        this.screenTexts = await queryScreenText(
          this.trace.engine,
          this.ts,
          upid !== null ? upid : undefined,
        );
      }
    }

    // Query 'Why recomposed' cause details if this is a scope or composable
    const causeRes = await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE android.ui_hierarchy;
      SELECT
        scope_id,
        name AS scope_name,
        state_id,
        state_type,
        state_value
      FROM android_ui_hierarchy_recomposition_cause
      WHERE scope_event_id = ${sel.eventId}
      LIMIT 10;
    `);
    const causeIt = causeRes.iter({
      scope_id: LONG_NULL,
      scope_name: STR_NULL,
      state_id: LONG_NULL,
      state_type: STR_NULL,
      state_value: STR_NULL,
    });
    const causes: RecompositionCauseInfo[] = [];
    for (; causeIt.valid(); causeIt.next()) {
      causes.push({
        scopeId: causeIt.scope_id ?? undefined,
        scopeName: causeIt.scope_name ?? undefined,
        stateId: causeIt.state_id ?? undefined,
        stateType: causeIt.state_type ?? undefined,
        stateValue: causeIt.state_value ?? undefined,
      });
    }
    this.causes = causes;
    m.redraw();
  }

  render(): m.Children {
    return m(
      DetailsShell,
      {
        title: `Compose: ${this.name || this.eventType}`,
        description: `Type: ${this.eventType}`,
        buttons:
          this.matchingWindow !== undefined &&
          this.session !== undefined &&
          m(
            'a',
            {
              href: `#!${UI_HIERARCHY_ROUTE}/${encodeURIComponent(this.matchingWindow.key)}`,
              title:
                'Open UI Hierarchy viewer for this process at this timestamp',
              onclick: (e: MouseEvent) => {
                e.preventDefault();
                void (async () => {
                  if (this.matchingWindow && this.session) {
                    await this.session.setWindow(this.matchingWindow.key);
                    if (this.ts !== undefined) {
                      await this.session.setNearestTs(this.ts);
                    }
                    this.trace.navigate(
                      `#!${UI_HIERARCHY_ROUTE}/${encodeURIComponent(this.matchingWindow.key)}`,
                    );
                  }
                })();
              },
            },
            m(Button, {
              label: 'View UI Hierarchy',
              icon: 'open_in_full',
              intent: Intent.Primary,
            }),
          ),
      },
      m(GridLayout, [
        m(
          Section,
          {title: 'Event Details'},
          m(Tree, [
            m(TreeNode, {left: 'Type', right: this.eventType}),
            m(TreeNode, {left: 'Name', right: this.name}),
            this.ts !== undefined &&
              m(TreeNode, {
                left: 'Timestamp',
                right: m(Timestamp, {
                  trace: this.trace,
                  ts: Time.fromRaw(this.ts),
                }),
              }),
            this.dur !== undefined &&
              this.dur > 0n &&
              m(TreeNode, {
                left: 'Duration',
                right: m(DurationWidget, {trace: this.trace, dur: this.dur}),
              }),
            this.scopeId !== undefined &&
              m(TreeNode, {left: 'Scope ID', right: `${this.scopeId}`}),
            this.stateId !== undefined &&
              m(TreeNode, {left: 'State Object ID', right: `${this.stateId}`}),
            this.value !== undefined &&
              m(TreeNode, {left: 'State Value', right: `"${this.value}"`}),
            (this.dirty1 !== undefined || this.dirty2 !== undefined) &&
              m(TreeNode, {
                left: 'Changed Bits ($changed)',
                right: `0x${(this.dirty1 ?? 0n).toString(16)} 0x${(this.dirty2 ?? 0n).toString(16)}`,
              }),
          ]),
        ),
        this.causes.length > 0 &&
          m(
            Section,
            {title: 'Why Recomposed (Invalidating State Causes)'},
            m('table.pf-uih-props-table', [
              m('thead', [
                m('tr', [
                  m('th', 'State ID'),
                  m('th', 'State Type'),
                  m('th', 'Value At Invalidation'),
                ]),
              ]),
              m('tbody', [
                this.causes.map((c) =>
                  m('tr', [
                    m(
                      'td.pf-uih-code',
                      c.stateId !== undefined
                        ? `#${c.stateId}`
                        : 'Explicit invalidate()',
                    ),
                    m('td', c.stateType ?? '-'),
                    m(
                      'td',
                      c.stateValue !== undefined ? `"${c.stateValue}"` : '-',
                    ),
                  ]),
                ),
              ]),
            ]),
          ),
        m(
          Section,
          {
            title: 'Associated Window & Process',
          },
          m(Tree, [
            this.matchingWindow !== undefined &&
              m(TreeNode, {
                left: 'Window',
                right: `${this.matchingWindow.title} (id: ${this.matchingWindow.windowId})`,
              }),
            this.matchingWindow !== undefined &&
              m(TreeNode, {
                left: 'Process',
                right: `${this.matchingWindow.processName ?? 'App'} (PID ${this.matchingWindow.pid}, UPID ${this.matchingWindow.upid})`,
              }),
            this.screenTexts.length > 0 &&
              m(TreeNode, {
                left: 'Visible Composable Elements',
                right: `${this.screenTexts.length} items in window`,
              }),
          ]),
        ),
        this.screenTexts.length > 0 &&
          m(
            Section,
            {
              title: `Visible Composable Elements (${this.screenTexts.length} in window)`,
            },
            m('table.pf-uih-props-table', [
              m('thead', [
                m('tr', [
                  m('th', 'Role'),
                  m('th', 'Text / Label'),
                  m('th', 'Tag'),
                  m('th', 'Clickable'),
                ]),
              ]),
              m('tbody', [
                this.screenTexts
                  .slice(0, 15)
                  .map((st) =>
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

export async function registerComposeEventTracks(
  trace: Trace,
  getProcessGroup: (
    upid: number,
    processName?: string,
    pid?: number,
  ) => TrackNode,
  session?: UiHierarchySession,
): Promise<void> {
  try {
    // Check if android_ui_hierarchy_compose_event exists and has rows
    const checkRes = await trace.engine.query(`
      INCLUDE PERFETTO MODULE android.ui_hierarchy;
      SELECT count(*) AS count FROM android_ui_hierarchy_compose_event LIMIT 1;
    `);
    const checkIt = checkRes.iter({count: NUM});
    if (!checkIt.valid() || checkIt.count === 0) return;

    // 1. Per-thread Composable Call / Scope Tracks (nested in the process group)
    const threadRes = await trace.engine.query(`
      INCLUDE PERFETTO MODULE android.ui_hierarchy;
      SELECT DISTINCT
        e.utid,
        t.name AS thread_name,
        t.tid,
        t.upid,
        p.name AS process_name,
        p.pid
      FROM android_ui_hierarchy_compose_event e
      JOIN thread t ON e.utid = t.utid
      LEFT JOIN process p ON t.upid = p.upid
      WHERE e.type IN ('composition', 'scope', 'composable_call')
      ORDER BY t.tid;
    `);
    const threadIt = threadRes.iter({
      utid: NUM,
      thread_name: STR_NULL,
      tid: NUM,
      upid: NUM,
      process_name: STR_NULL,
      pid: NUM_NULL,
    });

    for (; threadIt.valid(); threadIt.next()) {
      const utid = threadIt.utid;
      const tid = threadIt.tid;
      const upid = threadIt.upid;
      const threadName = threadIt.thread_name ?? `Thread ${tid}`;

      const typesRes = await trace.engine.query(`
        INCLUDE PERFETTO MODULE android.ui_hierarchy;
        SELECT
          count(CASE WHEN type IN ('composition', 'scope') THEN 1 END) AS scope_count,
          count(CASE WHEN type = 'composable_call' THEN 1 END) AS call_count
        FROM android_ui_hierarchy_compose_event
        WHERE utid = ${utid};
      `);
      const typesIt = typesRes.iter({
        scope_count: NUM,
        call_count: NUM,
      });
      const hasScopes = typesIt.valid() && typesIt.scope_count > 0;
      const hasCalls = typesIt.valid() && typesIt.call_count > 0;
      const procGroup = getProcessGroup(
        upid,
        threadIt.process_name ?? undefined,
        threadIt.pid ?? undefined,
      );

      if (hasScopes) {
        const scopeUri = `/ui_hierarchy_compose_scopes_${utid}`;
        const scopeSrc = `
          SELECT
            id,
            ts,
            iif(dur > 0, dur, 1) AS dur,
            iif(type = 'composition', 0, depth + 1) AS depth,
            iif(
              type = 'composition',
              'Composition Pass',
              'Recompose Scope: ' || coalesce(name, 'scope #' || scope_id)
            ) AS name
          FROM android_ui_hierarchy_compose_event
          WHERE utid = ${utid}
            AND type IN ('composition', 'scope')
        `;

        trace.tracks.registerTrack({
          uri: scopeUri,
          renderer: SliceTrack.create({
            trace,
            uri: scopeUri,
            dataset: new SourceDataset({
              schema: {id: NUM, ts: LONG, dur: LONG, depth: NUM, name: STR},
              src: scopeSrc,
            }),
            colorizer: (row) => materialColorScheme(row.name),
            detailsPanel: () => new ComposeEventDetailsPanel(trace, session),
          }),
        });

        procGroup.addChildInOrder(
          new TrackNode({
            uri: scopeUri,
            name: `Compose Scopes: ${threadName}`,
            sortOrder: -45,
          }),
        );
      }

      if (hasCalls) {
        const callUri = `/ui_hierarchy_compose_calls_${utid}`;
        const callSrc = `
          SELECT
            id,
            ts,
            iif(dur > 0, dur, 1) AS dur,
            depth,
            coalesce(name, 'Composable Call') AS name
          FROM android_ui_hierarchy_compose_event
          WHERE utid = ${utid}
            AND type = 'composable_call'
        `;

        trace.tracks.registerTrack({
          uri: callUri,
          renderer: SliceTrack.create({
            trace,
            uri: callUri,
            dataset: new SourceDataset({
              schema: {id: NUM, ts: LONG, dur: LONG, depth: NUM, name: STR},
              src: callSrc,
            }),
            colorizer: (row) => materialColorScheme(row.name),
            detailsPanel: () => new ComposeEventDetailsPanel(trace, session),
          }),
        });

        procGroup.addChildInOrder(
          new TrackNode({
            uri: callUri,
            name: `Composable Calls: ${threadName}`,
            sortOrder: -44,
          }),
        );
      }
    }

    // 2. State & Invalidation Events Track per process
    const stateProcRes = await trace.engine.query(`
      INCLUDE PERFETTO MODULE android.ui_hierarchy;
      SELECT DISTINCT
        e.upid,
        p.name AS process_name,
        p.pid
      FROM android_ui_hierarchy_compose_event e
      LEFT JOIN process p ON e.upid = p.upid
      WHERE e.type IN ('state_changed', 'scope_invalidated', 'scope_disposed');
    `);
    const stateProcIt = stateProcRes.iter({
      upid: NUM,
      process_name: STR_NULL,
      pid: NUM_NULL,
    });

    for (; stateProcIt.valid(); stateProcIt.next()) {
      const upid = stateProcIt.upid;
      const uri = `/ui_hierarchy_compose_state_events_${upid}`;
      const src = `
        SELECT
          id,
          ts,
          0 AS dur,
          0 AS depth,
          type || ': ' || coalesce(name, 'state #' || state_id, 'scope #' || scope_id) AS name
        FROM android_ui_hierarchy_compose_event
        WHERE upid = ${upid}
          AND type IN ('state_changed', 'scope_invalidated', 'scope_disposed')
      `;

      trace.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace,
          uri,
          dataset: new SourceDataset({
            schema: {id: NUM, ts: LONG, dur: LONG, depth: NUM, name: STR},
            src,
          }),
          colorizer: (row) => materialColorScheme(row.name),
          detailsPanel: () => new ComposeEventDetailsPanel(trace, session),
        }),
      });

      const procGroup = getProcessGroup(
        upid,
        stateProcIt.process_name ?? undefined,
        stateProcIt.pid ?? undefined,
      );
      procGroup.addChildInOrder(
        new TrackNode({
          uri,
          name: 'Compose: State & Invalidations',
          sortOrder: -43,
        }),
      );
    }

    // 3. Completeness & Skipped Frames Track per process
    const completenessRes = await trace.engine.query(`
      INCLUDE PERFETTO MODULE android.ui_hierarchy;
      SELECT DISTINCT
        w.upid,
        w.process_name,
        w.pid,
        w.window_id,
        w.window_name
      FROM __intrinsic_ui_hierarchy_window_frame f
      JOIN android_ui_hierarchy_window w ON f.window_id = w.window_id;
    `);
    const completenessIt = completenessRes.iter({
      upid: NUM,
      process_name: STR_NULL,
      pid: NUM_NULL,
      window_id: NUM,
      window_name: STR_NULL,
    });

    for (; completenessIt.valid(); completenessIt.next()) {
      const upid = completenessIt.upid;
      const windowId = completenessIt.window_id;
      const winName = completenessIt.window_name ?? `Window ${windowId}`;
      const uri = `/ui_hierarchy_completeness_track_${windowId}`;
      const src = `
        SELECT
          id,
          ts,
          0 AS dur,
          0 AS depth,
          CASE
            WHEN skipped_frames > 0 THEN 'Skipped ' || skipped_frames || ' frames'
            ELSE 'Frame #' || frame_number
          END AS name
        FROM __intrinsic_ui_hierarchy_window_frame
        WHERE window_id = ${windowId}
      `;

      trace.tracks.registerTrack({
        uri,
        renderer: SliceTrack.create({
          trace,
          uri,
          dataset: new SourceDataset({
            schema: {id: NUM, ts: LONG, dur: LONG, depth: NUM, name: STR},
            src,
          }),
          colorizer: (row) =>
            materialColorScheme(
              row.name.includes('Skipped') ? 'red_warning' : 'frame_marker',
            ),
          detailsPanel: () => new ComposeEventDetailsPanel(trace, session),
        }),
      });

      const procGroup = getProcessGroup(
        upid,
        completenessIt.process_name ?? undefined,
        completenessIt.pid ?? undefined,
      );
      procGroup.addChildInOrder(
        new TrackNode({
          uri,
          name: `UI Frames: ${winName}`,
          sortOrder: -42,
        }),
      );
    }
  } catch {
    // Graceful fallback if tables or views are not available
  }
}
