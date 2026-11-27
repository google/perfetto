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
import {
  MemoryOverviewPage,
  type MemoryTreePage,
} from './views/landing_page/memory_overview_page';
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
    // facts live in dedicated pages of the tree sidebar.
    if (hdeSession !== undefined) {
      hdeSession.setDefaultViewOverride('flamegraph');
      hdeSession.setHrefBuilder((sub) => {
        const activeUpid = hdeSession.activeDump?.upid ?? bestUpid;
        if (activeUpid === undefined) {
          return sub ? `#!/heapdump/${sub}` : '#!/heapdump';
        }
        return `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(activeUpid, 'java-hde', sub)}`;
      });
    }

    let autoNavigated = false;
    const hdeRestored = hdePlugin.wasRestoredFromPermalink();
    if (hdeRestored && hdeSession !== undefined) {
      const targetUpid = hdeSession.activeDump?.upid ?? bestUpid;
      if (targetUpid !== undefined) {
        const sub = hdeSession.navPath || undefined;
        trace.initialPage.suggest(
          `${MEMORY_PAGE_ROOT}/${formatMemorySubpage(targetUpid, 'java-hde', sub)}`,
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
          ? `${MEMORY_PAGE_ROOT}/${formatMemorySubpage(targetUpid, 'overview')}`
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
        activePage: parsed.page,
        hdeSession,
        hdeSubpage: parsed.hdeSubpage,
        autoNavigated,
        openByDefault,
        hideDefaultChangedHint,
        onUpidChange: (newUpid) => {
          const nextDump = hdeSession?.selectDumpForProcess(newUpid);
          // Preserve the current tree view across process switches, resetting
          // instance-specific HDE drill-downs to a safe view.
          let nextPage = parsed.page;
          let nextHdeSub = parsed.hdeSubpage;
          if (nextPage === 'java-hde') {
            if (nextDump === undefined) {
              nextPage = 'overview';
              nextHdeSub = undefined;
            } else if (
              nextHdeSub?.startsWith('object') ||
              nextHdeSub?.startsWith('flamegraph_objects')
            ) {
              nextHdeSub = 'flamegraph';
            }
          }
          trace.navigate(
            `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(newUpid, nextPage, nextHdeSub)}`,
          );
        },
        onNavigatePage: (page: MemoryTreePage, hdeSubpage?: string) => {
          if (upid === undefined || Number.isNaN(upid)) return;
          trace.navigate(
            `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(upid, page, hdeSubpage)}`,
          );
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
        page: 'java-hde',
        hdeSubpage: cleanedSubpage || undefined,
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
  readonly page: MemoryTreePage;
  readonly hdeSubpage?: string;
}

function parseMemorySubpage(subpage: string | undefined): MemorySubpage {
  if (!subpage) {
    return {page: 'overview'};
  }
  const parts = subpage.split('/').filter((x) => x !== '');
  const first = parts[0];
  if (first === undefined) {
    return {page: 'overview'};
  }
  const upid = parseInt(first, 10);
  const safeUpid = Number.isNaN(upid) ? Number.NaN : upid;
  const seg1 = parts[1];
  const seg2 = parts[2];

  if (seg1 === 'smaps') {
    return {upid: safeUpid, page: 'smaps'};
  }
  if (seg1 === 'java') {
    if (seg2 === 'bitmaps') {
      return {upid: safeUpid, page: 'java-bitmaps'};
    }
    return {upid: safeUpid, page: 'java-overview'};
  }
  if (seg1 === 'native') {
    if (seg2 === 'flamegraph') {
      return {upid: safeUpid, page: 'native-flamegraph'};
    }
    return {upid: safeUpid, page: 'native-overview'};
  }
  if (seg1 === 'heap') {
    const rest = parts.slice(2).join('/');
    return {
      upid: safeUpid,
      page: 'java-hde',
      hdeSubpage: rest !== '' ? rest : undefined,
    };
  }
  return {upid: safeUpid, page: 'overview'};
}

function formatMemorySubpage(
  upid: number,
  page: MemoryTreePage,
  hdeSubpage?: string,
): string {
  switch (page) {
    case 'overview':
      return `${upid}`;
    case 'smaps':
      return `${upid}/smaps`;
    case 'java-overview':
      return `${upid}/java`;
    case 'java-bitmaps':
      return `${upid}/java/bitmaps`;
    case 'native-overview':
      return `${upid}/native`;
    case 'native-flamegraph':
      return `${upid}/native/flamegraph`;
    case 'java-hde':
      return hdeSubpage ? `${upid}/heap/${hdeSubpage}` : `${upid}/heap`;
  }
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
        `#!${pageRoot}/${formatMemorySubpage(bestUpid, parsed.page, parsed.hdeSubpage)}`,
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
