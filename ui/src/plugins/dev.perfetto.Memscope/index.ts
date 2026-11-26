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

import './styles.scss';
import m from 'mithril';
import {z} from 'zod';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Setting} from '../../public/settings';
import type {Trace} from '../../public/trace';
import {NUM} from '../../trace_processor/query_result';
import {EmptyState} from '../../widgets/empty_state';
import HeapDumpExplorerPlugin from '../com.android.HeapDumpExplorer';
import HeapProfilePlugin, {
  traceHasTimelineData,
} from '../dev.perfetto.HeapProfile';
import RecordPageV2 from '../dev.perfetto.RecordTraceV2';
import {LiveSession} from './sessions/live_session';
import {ConnectionPage} from './views/connection';
import {Dashboard} from './views/dashboard';
import {MemoryOverviewPage} from './views/landing_page/memory_overview_page';
import {
  defaultTabForProc,
  formatMemorySubpage,
  MEMORY_PAGE_ROOT,
  type MemorySubpage,
  type MemoryTab,
  parseMemorySubpage,
  resolveTabForProc,
} from './views/landing_page/memory_route';
import {
  loadProcessMemoryStats,
  pickBestProc,
  type ProcMemStat,
} from './views/landing_page/proc_mem_stats';

export default class MemscopePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Memscope';
  static readonly description =
    'Live memory profiler for Android/Linux devices';
  static readonly dependencies = [
    RecordPageV2,
    HeapProfilePlugin,
    HeapDumpExplorerPlugin,
  ];
  private static openByDefaultSetting: Setting<boolean>;
  private static hideDefaultChangedHintSetting: Setting<boolean>;

  static onActivate(app: App) {
    MemscopePlugin.openByDefaultSetting = app.settings.register({
      id: 'dev.perfetto.OpenMemoryOverviewByDefault',
      name: 'Open Memory Overview by default',
      description:
        'Open traces containing smaps snapshots in Memory Overview instead ' +
        'of the timeline.',
      schema: z.boolean(),
      defaultValue: true,
    });

    MemscopePlugin.hideDefaultChangedHintSetting = app.settings.register({
      id: 'dev.perfetto.HideMemoryOverviewDefaultChangedHint',
      name: 'Hide Memory Overview default-page explanation',
      description:
        'Do not show the explanation that Memory Overview is the default ' +
        'page for traces containing smaps snapshots.',
      schema: z.boolean(),
      defaultValue: false,
      headless: true,
    });

    let session: LiveSession | undefined;

    app.sidebar.addMenuItem({
      section: 'trace_files',
      text: 'Memscope',
      href: '#!/memscope',
      icon: 'memory',
      sortOrder: 2.5,
      badge: 'preview',
    });

    app.pages.registerPage({
      route: '/memscope',
      render: () => {
        if (session) {
          return m(Dashboard, {
            app,
            session,
            onStopped: () => {
              session?.dispose();
              session = undefined;
            },
          });
        } else {
          return m(ConnectionPage, {
            app,
            onConnected: (result) => {
              session = new LiveSession(app, result);
              session.onSnapshot(() => m.redraw());
            },
          });
        }
      },
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    const openByDefault = MemscopePlugin.openByDefaultSetting;
    const hideDefaultChangedHint = MemscopePlugin.hideDefaultChangedHintSetting;
    const availability = await this.getMemoryAvailability(trace);
    const procs = await loadProcessMemoryStats(trace.engine);
    const bestProc = pickBestProc(procs);
    const hdePlugin = trace.plugins.getPlugin(HeapDumpExplorerPlugin);
    const hdeSession = hdePlugin.getSession();

    // Route all Heap Dump Explorer links and navigation through the unified
    // /memory/<upid>/heap/... workbench so the single sidebar item stays active
    // and browser history works cleanly across all memory scopes.
    hdeSession.setHrefBuilder((sub, dump) => {
      const upid = dump?.upid ?? bestProc?.upid;
      if (upid === undefined) return `#!${MEMORY_PAGE_ROOT}`;
      return `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(upid, 'heap', sub)}`;
    });

    const autoNavigatedOverview =
      openByDefault.get() && availability.hasSmapsSnapshots;
    const autoNavigatedHeap =
      !autoNavigatedOverview &&
      availability.hasHeapDumps &&
      HeapProfilePlugin.openHeapDumpExplorerByDefaultFlag.get() &&
      !(await traceHasTimelineData(trace));
    if (autoNavigatedHeap) {
      hdeSession.autoNavigated = true;
    }

    trace.pages.registerPage({
      route: MEMORY_PAGE_ROOT,
      render: (subpage) => {
        const {parsed, redirect} = resolveMemoryRoute(subpage, bestProc);
        if (redirect !== undefined) {
          return redirect;
        }

        const currentTab: MemoryTab = parsed.tab ?? 'summary';
        return m(MemoryOverviewPage, {
          trace,
          procs,
          upid: parsed.upid,
          tab: currentTab,
          hdeSubpage: parsed.hdeSubpage,
          hdeSession,
          autoNavigated: autoNavigatedOverview,
          hdeAvailable: availability.hasHeapDumps,
          openByDefault,
          hideDefaultChangedHint,
          onUpidChange: (newUpid) => {
            const targetProc = procs.find((p) => p.upid === newUpid);
            const nextTab = resolveTabForProc(currentTab, targetProc);
            // Keep static HDE views (e.g. Flamegraph, Bitmaps, Classes) when
            // switching processes, but drop dump-specific object/flamegraph
            // inspectors belonging to the previous process.
            let nextHdeSub: string | undefined;
            if (nextTab === 'heap') {
              const curView = hdeSession.nav.view;
              if (curView !== 'object' && curView !== 'flamegraph-objects') {
                nextHdeSub = curView;
              }
            }
            trace.navigate(
              `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(newUpid, nextTab, nextHdeSub)}`,
            );
          },
          onTabChange: (tab, explicitHdeSub) => {
            if (parsed.upid === undefined) return;
            const hdeSub =
              tab === 'heap'
                ? (explicitHdeSub ??
                  (hdeSession.activeDump?.upid === parsed.upid &&
                  hdeSession.navPath !== ''
                    ? hdeSession.navPath
                    : undefined))
                : undefined;
            trace.navigate(
              `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(parsed.upid, tab, hdeSub)}`,
            );
          },
        });
      },
    });

    // Legacy route aliases so bookmarks and external links to /memoryoverview
    // and /heapdump continue to land in the right scope of /memory.
    trace.pages.registerPage({
      route: '/memoryoverview',
      render: (subpage) => {
        const clean = subpage?.replace(/^\/+/, '');
        location.replace(
          `#!${MEMORY_PAGE_ROOT}${clean !== undefined && clean !== '' ? '/' + clean : ''}`,
        );
        return m(EmptyState, {
          icon: 'hourglass',
          title: 'Redirecting to Memory...',
          fillHeight: true,
        });
      },
    });

    trace.pages.registerPage({
      route: '/heapdump',
      render: (subpage) => {
        const clean = subpage?.replace(/^\/+/, '');
        const targetUpid =
          hdeSession.activeDump?.upid ??
          procs.find((p) => p.heapDumps > 0)?.upid ??
          bestProc?.upid;
        if (targetUpid === undefined) {
          return m(EmptyState, {
            title: 'No Java heap dumps in this trace',
            fillHeight: true,
          });
        }
        location.replace(
          `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(targetUpid, 'heap', clean !== undefined && clean !== '' ? clean : undefined)}`,
        );
        return m(EmptyState, {
          icon: 'hourglass',
          title: 'Redirecting to Memory...',
          fillHeight: true,
        });
      },
    });

    if (
      availability.hasSmapsSnapshots ||
      availability.hasHeapDumps ||
      availability.hasNativeProfiles
    ) {
      trace.sidebar.addMenuItem({
        section: 'current_trace',
        sortOrder: 25,
        text: 'Memory',
        href: `#!${MEMORY_PAGE_ROOT}`,
        icon: 'memory',
      });
    }

    if (hdeSession.restored && hdeSession.activeDump !== null) {
      // Restored from a shared Heap Dump Explorer permalink: land on the exact
      // saved process, dump, and tab (beats default-open suggestions).
      const sub = hdeSession.navPath !== '' ? hdeSession.navPath : undefined;
      const route = `${MEMORY_PAGE_ROOT}/${formatMemorySubpage(hdeSession.activeDump.upid, 'heap', sub)}`;
      trace.initialPage.suggest(route, 600);
    } else if (autoNavigatedOverview) {
      trace.initialPage.suggest(MEMORY_PAGE_ROOT, 500);
    } else if (autoNavigatedHeap) {
      const targetUpid = hdeSession.activeDump?.upid ?? bestProc?.upid;
      const route =
        targetUpid !== undefined
          ? `${MEMORY_PAGE_ROOT}/${formatMemorySubpage(targetUpid, 'heap')}`
          : MEMORY_PAGE_ROOT;
      trace.initialPage.suggest(route, 100);
    }
  }

  private async getMemoryAvailability(trace: Trace): Promise<{
    readonly hasSmapsSnapshots: boolean;
    readonly hasHeapDumps: boolean;
    readonly hasNativeProfiles: boolean;
  }> {
    const result = await trace.engine.query(`
      SELECT
        EXISTS(SELECT 1 FROM profiler_smaps) AS hasSmapsSnapshots,
        EXISTS(SELECT 1 FROM heap_graph) AS hasHeapDumps,
        EXISTS(SELECT 1 FROM heap_profile_allocation) AS hasNativeProfiles
    `);
    const row = result.firstRow({
      hasSmapsSnapshots: NUM,
      hasHeapDumps: NUM,
      hasNativeProfiles: NUM,
    });
    return {
      hasSmapsSnapshots: row.hasSmapsSnapshots !== 0,
      hasHeapDumps: row.hasHeapDumps !== 0,
      hasNativeProfiles: row.hasNativeProfiles !== 0,
    };
  }
}

function resolveMemoryRoute(
  subpage: string | undefined,
  bestProc: ProcMemStat | undefined,
): {parsed: MemorySubpage; redirect?: m.Children} {
  const parsed = parseMemorySubpage(subpage);

  if (parsed.upid === undefined) {
    if (bestProc !== undefined) {
      const targetTab = parsed.tab ?? defaultTabForProc(bestProc);
      location.replace(
        `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(bestProc.upid, targetTab)}`,
      );
      return {
        parsed,
        redirect: m(EmptyState, {
          icon: 'hourglass',
          title: 'Loading process...',
          fillHeight: true,
        }),
      };
    }
    return {
      parsed,
      redirect: m(EmptyState, {
        title: 'No processes with memory in this trace',
        fillHeight: true,
      }),
    };
  }

  return {parsed};
}
