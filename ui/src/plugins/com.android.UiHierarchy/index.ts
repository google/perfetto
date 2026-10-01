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
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {registerComposeEventTracks} from './ui_hierarchy_events_track';
import {UiHierarchyPage} from './ui_hierarchy_page';
import {UI_HIERARCHY_ROUTE} from './ui_hierarchy_route';
import {UiHierarchySession} from './ui_hierarchy_session';
import {createUiHierarchyTrack} from './ui_hierarchy_track';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.UiHierarchy';
  static readonly description =
    'Android UI Hierarchy viewer: multi-window timeline tracks with lazy queries, ' +
    '3-pane 2D/3D hierarchy inspection, and node version change tracking.';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const session = new UiHierarchySession(ctx);
    await session.init();
    if (session.windows.length === 0) return;

    // Full-screen page + sidebar navigation
    ctx.pages.registerPage({
      route: UI_HIERARCHY_ROUTE,
      render: (subpage) => m(UiHierarchyPage, {session, subpage}),
    });

    ctx.sidebar.addMenuItem({
      section: 'current_trace',
      sortOrder: 36,
      text: 'UI Hierarchy',
      href: `#!${UI_HIERARCHY_ROUTE}`,
      icon: 'layers',
    });

    const processGroupsPlugin = ctx.plugins.getPlugin(
      ProcessThreadGroupsPlugin,
    );
    const fallbackGroups = new Map<number, TrackNode>();

    const getOrCreateGroupForProcess = (
      upid: number,
      processName?: string,
      pid?: number,
    ): TrackNode => {
      const existing = processGroupsPlugin.getGroupForProcess(upid);
      if (existing) return existing;

      let fallback = fallbackGroups.get(upid);
      if (!fallback) {
        const procLabel =
          processName ?? (pid !== undefined ? `PID ${pid}` : `Process ${upid}`);
        fallback = new TrackNode({
          name: `${procLabel} (UI Hierarchy)`,
          isSummary: true,
          sortOrder: -40,
        });
        fallback.expand();
        fallbackGroups.set(upid, fallback);
        ctx.defaultWorkspace.addChildInOrder(fallback);
      }
      return fallback;
    };

    // Register per-window UI hierarchy tracks nested in each process's track group
    for (const win of session.windows) {
      const uri = `/ui_hierarchy_track/${win.key}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: createUiHierarchyTrack(ctx, uri, win, session),
      });
      const name = `UI Hierarchy: ${win.title}`;
      const group = getOrCreateGroupForProcess(
        win.upid,
        win.processName,
        win.pid,
      );
      group.addChildInOrder(new TrackNode({uri, name, sortOrder: -45}));
    }

    // Register Compose runtime events, scope slices, and state invalidations per-process
    await registerComposeEventTracks(ctx, getOrCreateGroupForProcess, session);
  }
}
