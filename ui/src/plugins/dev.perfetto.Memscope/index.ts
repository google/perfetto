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
import type {HeapDump} from '../com.android.HeapDumpExplorer/types';
import HeapProfilePlugin, {
  traceHasTimelineData,
} from '../dev.perfetto.HeapProfile';
import RecordPageV2 from '../dev.perfetto.RecordTraceV2';
import {LiveSession} from './sessions/live_session';
import {ConnectionPage} from './views/connection';
import {Dashboard} from './views/dashboard';
import {MemoryOverviewPage} from './views/landing_page/memory_overview_page';
import type {MemoryWorkbenchSection} from './views/landing_page/proc_mem_overview';
import {getBestProcess} from './views/landing_page/proc_mem_stats';

const MEMORY_PAGE_ROOT = '/memory';

export default class MemscopePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Memscope';
  static readonly description =
    'Live memory profiler and unified Memory Workbench (process overview, ' +
    'smaps VMAs, native allocations, and Java heap dump explorer)';
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
        'Open traces containing smaps snapshots in Memory Workbench instead ' +
        'of the timeline.',
      schema: z.boolean(),
      defaultValue: true,
    });

    MemscopePlugin.hideDefaultChangedHintSetting = app.settings.register({
      id: 'dev.perfetto.HideMemoryOverviewDefaultChangedHint',
      name: 'Hide Memory default-page explanation',
      description:
        'Do not show the explanation that Memory Workbench is the default ' +
        'page for traces containing smaps snapshots or heap dumps.',
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

    const defaultSection: MemoryWorkbenchSection =
      !availability.hasSmapsSnapshots &&
      !availability.hasNativeProfiles &&
      availability.hasHeapDumps
        ? 'heap'
        : 'overview';

    // Route internal Heap Dump Explorer navigations into /memory/<upid>/heap/...
    if (hdeSession !== undefined) {
      hdeSession.setHrefBuilder((sub) => {
        const activeUpid = hdeSession.activeDump?.upid ?? bestUpid;
        if (activeUpid === undefined) {
          return sub ? `#!/heapdump/${sub}` : '#!/heapdump';
        }
        return `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(activeUpid, 'heap', sub)}`;
      });
    }

    let autoNavigated = false;
    const hdeRestored = hdePlugin.wasRestoredFromPermalink();
    if (hdeRestored && hdeSession !== undefined) {
      const targetUpid = hdeSession.activeDump?.upid ?? bestUpid;
      if (targetUpid !== undefined) {
        const sub = hdeSession.navPath || undefined;
        trace.initialPage.suggest(
          `${MEMORY_PAGE_ROOT}/${formatMemorySubpage(targetUpid, 'heap', sub)}`,
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
          ? `${MEMORY_PAGE_ROOT}/${formatMemorySubpage(targetUpid, 'heap')}`
          : MEMORY_PAGE_ROOT;
      trace.initialPage.suggest(route, 200);
    }

    const renderWorkbench = (parsed: MemorySubpage): m.Children => {
      const upid = parsed.upid;
      const section = parsed.section;

      if (
        section === 'heap' &&
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
        section,
        hdeSession,
        hdeSubpage: section === 'heap' ? parsed.hdeSubpage : undefined,
        autoNavigated,
        openByDefault,
        hideDefaultChangedHint,
        onUpidChange: (newUpid) => {
          let nextSection = section;
          let nextHdeSubpage: string | undefined;
          if (section === 'heap' && hdeSession !== undefined) {
            const matched = hdeSession.selectDumpForProcess(newUpid);
            if (matched === undefined) {
              nextSection = 'overview';
            } else {
              nextHdeSubpage = hdeSession.navPath || undefined;
            }
          }
          trace.navigate(
            `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(newUpid, nextSection, nextHdeSubpage)}`,
          );
        },
        onSelectSection: (nextSection) => {
          if (upid === undefined || Number.isNaN(upid)) return;
          let nextHdeSubpage: string | undefined;
          if (nextSection === 'heap' && hdeSession !== undefined) {
            hdeSession.selectDumpForProcess(upid);
            nextHdeSubpage = hdeSession.navPath || undefined;
          }
          trace.navigate(
            `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(upid, nextSection, nextHdeSubpage)}`,
          );
        },
        onSelectHeapView: (view: NavView, preferredTs?: time) => {
          if (
            upid === undefined ||
            Number.isNaN(upid) ||
            hdeSession === undefined
          ) {
            return;
          }
          hdeSession.selectDumpForProcess(upid, preferredTs);
          hdeSession.navigate(view);
        },
        onSelectDump: (dump: HeapDump, jumpToHeap: boolean) => {
          if (hdeSession === undefined) return;
          hdeSession.selectDump(dump);
          if (jumpToHeap || section === 'heap') {
            const sub = hdeSession.navPath || undefined;
            trace.navigate(
              `#!${MEMORY_PAGE_ROOT}/${formatMemorySubpage(dump.upid, 'heap', sub)}`,
            );
          }
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
        defaultSection,
        MEMORY_PAGE_ROOT,
      );
      if (redirect !== undefined) {
        return redirect;
      }
      return renderWorkbench(parsed);
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

    // Legacy /heapdump alias (bookmarks, permalinks, HeapProfile timeline drill-down).
    trace.pages.registerPage({
      route: '/heapdump',
      render: (subpage) => {
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
        return renderWorkbench({
          upid,
          section: 'heap',
          hdeSubpage: cleanedSubpage || undefined,
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
  readonly section: MemoryWorkbenchSection;
  readonly hdeSubpage?: string;
}

function parseMemorySubpage(
  subpage: string | undefined,
  defaultSection: MemoryWorkbenchSection,
): MemorySubpage {
  if (!subpage) {
    return {section: defaultSection};
  }
  const parts = subpage.split('/').filter((x) => x !== '');
  const first = parts[0];
  if (first === undefined) {
    return {section: defaultSection};
  }
  const upid = parseInt(first, 10);
  const secPart = parts[1];
  let section: MemoryWorkbenchSection;
  let hdeSubpage: string | undefined;
  if (secPart === 'smaps') {
    section = 'smaps';
  } else if (secPart === 'native') {
    section = 'native';
  } else if (secPart === 'heap') {
    section = 'heap';
    const rest = parts.slice(2).join('/');
    hdeSubpage = rest !== '' ? rest : undefined;
  } else if (secPart === 'overview' || secPart === 'summary') {
    section = 'overview';
  } else {
    section = defaultSection;
  }
  return {
    upid: Number.isNaN(upid) ? Number.NaN : upid,
    section,
    hdeSubpage,
  };
}

function formatMemorySubpage(
  upid: number,
  section: MemoryWorkbenchSection,
  hdeSubpage?: string,
): string {
  if (section === 'overview') {
    return `${upid}/overview`;
  }
  if (section === 'smaps') {
    return `${upid}/smaps`;
  }
  if (section === 'native') {
    return `${upid}/native`;
  }
  return hdeSubpage ? `${upid}/heap/${hdeSubpage}` : `${upid}/heap`;
}

function resolveMemoryRoute(
  subpage: string | undefined,
  bestUpid: number | undefined,
  defaultSection: MemoryWorkbenchSection,
  pageRoot: string,
): {parsed: MemorySubpage; redirect?: m.Children} {
  const parsed = parseMemorySubpage(subpage, defaultSection);

  if (parsed.upid === undefined) {
    if (bestUpid !== undefined) {
      location.replace(
        `#!${pageRoot}/${formatMemorySubpage(bestUpid, parsed.section, parsed.hdeSubpage)}`,
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
