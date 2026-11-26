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
import type {time} from '../../base/time';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Setting} from '../../public/settings';
import type {Trace} from '../../public/trace';
import {NUM} from '../../trace_processor/query_result';
import {EmptyState} from '../../widgets/empty_state';
import HeapDumpExplorerPlugin from '../com.android.HeapDumpExplorer';
import type {NavView} from '../com.android.HeapDumpExplorer/nav_state';
import HeapProfilePlugin, {
  traceHasTimelineData,
} from '../dev.perfetto.HeapProfile';
import RecordPageV2 from '../dev.perfetto.RecordTraceV2';
import {LiveSession} from './sessions/live_session';
import {ConnectionPage} from './views/connection';
import {Dashboard} from './views/dashboard';
import {MemoryOverviewPage} from './views/landing_page/memory_overview_page';
import {getBestProcess} from './views/landing_page/proc_mem_stats';

const MEMORY_PAGE_ROOT = '/memory';

export default class MemscopePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Memscope';
  static readonly description =
    'Live memory profiler and timeline-driven unified Memory page';
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
      name: 'Open Memory by default',
      description:
        'Open traces containing smaps snapshots in Memory instead of the ' +
        'timeline.',
      schema: z.boolean(),
      defaultValue: true,
    });

    MemscopePlugin.hideDefaultChangedHintSetting = app.settings.register({
      id: 'dev.perfetto.HideMemoryOverviewDefaultChangedHint',
      name: 'Hide Memory default-page explanation',
      description:
        'Do not show the explanation that Memory is the default page for ' +
        'traces containing smaps snapshots or heap dumps.',
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
    const hdePlugin = trace.plugins.getPlugin(HeapDumpExplorerPlugin);
    const hdeSession = hdePlugin.hasHeapDumps()
      ? hdePlugin.getSession()
      : undefined;
    const bestUpid = await getBestProcess(trace.engine);

    // Keep HDE internal tab & drill-down URLs inside /memory/<upid>/heap/...
    // and default embedded HDE straight to Flamegraph since Overview summary
    // facts live in dedicated sections above.
    if (hdeSession !== undefined) {
      hdeSession.setDefaultViewOverride('flamegraph');
      hdeSession.setHrefBuilder((sub) => {
        const activeUpid = hdeSession.activeDump?.upid ?? bestUpid;
        if (activeUpid === undefined) {
          return sub ? `#!/heapdump/${sub}` : '#!/heapdump';
        }
        return `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(activeUpid, sub)}`;
      });
    }

    let autoNavigated = false;
    const hdeRestored = hdePlugin.wasRestoredFromPermalink();
    if (hdeRestored && hdeSession !== undefined) {
      const targetUpid = hdeSession.activeDump?.upid ?? bestUpid;
      if (targetUpid !== undefined) {
        const sub = hdeSession.navPath || undefined;
        trace.initialPage.suggest(
          `${MEMORY_PAGE_ROOT}/${formatMemorySubpage(targetUpid, sub)}`,
          600,
        );
      }
    } else if (openByDefault.get() && availability.hasSmapsSnapshots) {
      autoNavigated = true;
      trace.initialPage.suggest(MEMORY_PAGE_ROOT, 500);
    } else if (
      availability.hasHeapDumps &&
      HeapProfilePlugin.openHeapDumpExplorerByDefaultFlag.get() &&
      !(await traceHasTimelineData(trace))
    ) {
      autoNavigated = true;
      const targetUpid = hdeSession?.activeDump?.upid ?? bestUpid;
      const route =
        targetUpid !== undefined
          ? `${MEMORY_PAGE_ROOT}/${formatMemorySubpage(targetUpid)}`
          : MEMORY_PAGE_ROOT;
      trace.initialPage.suggest(route, 250);
    }

    const renderPage = (parsed: MemorySubpage): m.Children => {
      const upid = parsed.upid;
      if (
        upid !== undefined &&
        !Number.isNaN(upid) &&
        hdeSession !== undefined &&
        hdeSession.activeDump?.upid !== upid
      ) {
        hdeSession.selectDumpForProcess(upid);
      }

      return m(MemoryOverviewPage, {
        trace,
        upid,
        hdeSession,
        hdeSubpage: parsed.hdeSubpage,
        focusHeapExplorer: parsed.focusHeapExplorer,
        autoNavigated,
        openByDefault,
        hideDefaultChangedHint,
        onUpidChange: (newUpid) => {
          hdeSession?.selectDumpForProcess(newUpid);
          trace.navigate(`#!${MEMORY_PAGE_ROOT}/${newUpid}`);
        },
        onOpenJavaHeap: (
          ts: time,
          view: NavView,
          params?: Record<string, unknown>,
        ) => {
          if (
            upid === undefined ||
            Number.isNaN(upid) ||
            hdeSession === undefined
          ) {
            return;
          }
          hdeSession.selectDumpForProcess(upid, ts);
          hdeSession.navigateWithTabs(view, params);
        },
      });
    };

    const renderFromMemoryRoute = (subpage: string | undefined): m.Children => {
      const {parsed, redirect} = resolveMemoryRoute(
        subpage,
        bestUpid,
        MEMORY_PAGE_ROOT,
      );
      if (redirect !== undefined) {
        return redirect;
      }
      return renderPage(parsed);
    };

    trace.pages.registerPage({
      route: MEMORY_PAGE_ROOT,
      render: renderFromMemoryRoute,
    });

    // Legacy /memoryoverview alias.
    trace.pages.registerPage({
      route: '/memoryoverview',
      render: renderFromMemoryRoute,
    });

    // Legacy /heapdump alias: delegate through HeapDumpExplorerPlugin when it
    // registered /heapdump so we never double-register the route, and hide its
    // standalone sidebar item.
    const renderHeapdumpAlias = (subpage: string | undefined): m.Children => {
      const upid = hdeSession?.activeDump?.upid ?? bestUpid;
      if (upid === undefined) {
        return m(EmptyState, {
          icon: 'memory',
          title: 'No processes with memory data in this trace',
        });
      }
      const cleanedSubpage = subpage?.startsWith('/')
        ? subpage.slice(1)
        : subpage;
      return renderPage({
        upid,
        hdeSubpage: cleanedSubpage || undefined,
        focusHeapExplorer: true,
      });
    };

    if (hdePlugin.hasHeapDumps()) {
      hdePlugin.setUnifiedHost(renderHeapdumpAlias);
    } else {
      trace.pages.registerPage({
        route: '/heapdump',
        render: renderHeapdumpAlias,
      });
    }

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
        badge: 'preview',
      });
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

interface MemorySubpage {
  readonly upid?: number;
  readonly hdeSubpage?: string;
  readonly focusHeapExplorer: boolean;
}

function parseMemorySubpage(subpage: string | undefined): MemorySubpage {
  if (!subpage) {
    return {focusHeapExplorer: false};
  }
  const parts = subpage.split('/').filter((x) => x !== '');
  const first = parts[0];
  if (first === undefined) {
    return {focusHeapExplorer: false};
  }
  const upid = parseInt(first, 10);
  if (parts[1] === 'heap') {
    const rest = parts.slice(2).join('/');
    return {
      upid: Number.isNaN(upid) ? Number.NaN : upid,
      hdeSubpage: rest !== '' ? rest : undefined,
      focusHeapExplorer: true,
    };
  }
  return {
    upid: Number.isNaN(upid) ? Number.NaN : upid,
    focusHeapExplorer: false,
  };
}

function formatMemorySubpage(upid: number, hdeSubpage?: string): string {
  return hdeSubpage ? `${upid}/heap/${hdeSubpage}` : `${upid}`;
}

function resolveMemoryRoute(
  subpage: string | undefined,
  bestUpid: number | undefined,
  pageRoot: string,
): {parsed: MemorySubpage; redirect?: m.Children} {
  const parsed = parseMemorySubpage(subpage);

  if (parsed.upid === undefined) {
    if (bestUpid !== undefined) {
      location.replace(
        `#!${pageRoot}/${formatMemorySubpage(bestUpid, parsed.hdeSubpage)}`,
      );
      return {
        parsed,
        redirect: m(EmptyState, {
          icon: 'hourglass',
          title: 'Loading process...',
        }),
      };
    }
    return {
      parsed,
      redirect: m(EmptyState, {
        icon: 'memory',
        title: 'No processes with memory data in this trace',
      }),
    };
  }

  return {parsed};
}
