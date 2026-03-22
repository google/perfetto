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
import {App} from '../../public/app';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {
  PINNED_PROCESSES,
  aggregateSmaps,
  type ProcessInfo,
  type SmapsEntry,
  type SmapsRollup,
  type ProcessStringsResult,
} from './smaps_connection';
import {
  TAB_PROCESSES,
  TAB_VMAS,
  TAB_STRINGS_DUPS,
  TAB_INSPECT,
  procTabKey,
  mapTabKey,
  vmapTabKey,
  getStore,
  newProcessTabState,
  newMappingTabState,
  newVmaMappingTabState,
  type StringsState,
  type MappingTabState,
  type ProcessTabState,
  type VmaMappingTabState,
  type PageContext,
} from './state';
import {renderProcessView} from './process_view';
import {renderVmaView} from './vma_view';

// ── Page component ──────────────────────────────────────────────────────────

interface SmapsExplorerPageAttrs {
  app: App;
}

export class SmapsExplorerPage
  implements m.ClassComponent<SmapsExplorerPageAttrs>
{
  private readonly s = getStore();
  private connectStatus: string | null = null;
  private error: string | null = null;
  private loadingPid: number | null = null;
  private enriching = false;
  private enrichProgress: {done: number; total: number} | null = null;
  private scanningAllSmaps = false;
  private scanAllProgress: {done: number; total: number} | null = null;
  private enrichGeneration = 0;
  private smapsScanGeneration = 0;

  // PageContext for extracted view modules — all properties are live
  // getters that read from the class instance or the shared store.
  private readonly ctx: PageContext = this.buildContext();

  private buildContext(): PageContext {
    const self = this;
    return {
      get processes() {
        return self.s.processes;
      },
      get smapsData() {
        return self.s.smapsData;
      },
      get rollups() {
        return self.s.rollups;
      },
      get vmaFilters() {
        return self.s.vmaFilters;
      },
      get isRoot() {
        return self.s.conn.isRoot;
      },
      get loadingPid() {
        return self.loadingPid;
      },
      get enrichGeneration() {
        return self.enrichGeneration;
      },
      get smapsScanGeneration() {
        return self.smapsScanGeneration;
      },
      get scanningAllSmaps() {
        return self.scanningAllSmaps;
      },
      s: self.s,
      inspectProcess: (pid) => self.inspectProcess(pid),
      openMapping: (ps, name) => self.openMapping(ps, name),
      openVmaProcesses: (name) => self.openVmaProcesses(name),
      openVmaProcDetail: (vs, pid) => self.openVmaProcDetail(vs, pid),
      scanSingleVma: (pid, ms, a, b, p) => self.scanSingleVma(pid, ms, a, b, p),
      startStringsScan: (pid, n, ps) => self.startStringsScan(pid, n, ps),
      captureHeap: (pid, n, app) => self.captureHeap(pid, n, app),
      scanAllSmaps: () => self.scanAllSmaps(),
      setVmaFilters: (f) => {
        self.s.vmaFilters = f;
      },
      getProcessStringsState: (ps) => self.getProcessStringsState(ps),
    };
  }

  // Convenience accessors
  private get conn() {
    return this.s.conn;
  }
  private get processes() {
    return this.s.processes;
  }
  private set processes(v: ProcessInfo[] | null) {
    this.s.processes = v;
  }
  private get rollups() {
    return this.s.rollups;
  }
  private set rollups(v: Map<number, SmapsRollup>) {
    this.s.rollups = v;
  }
  private get smapsData() {
    return this.s.smapsData;
  }

  /** Get or create per-process tab state */
  private getProcessState(pid: number): ProcessTabState {
    let ps = this.s.openProcesses.get(pid);
    if (ps === undefined) {
      ps = newProcessTabState();
      this.s.openProcesses.set(pid, ps);
      if (!this.s.openProcessOrder.includes(pid)) {
        this.s.openProcessOrder.push(pid);
      }
    }
    return ps;
  }

  private processStringsStates = new WeakMap<ProcessTabState, StringsState>();
  private getProcessStringsState(ps: ProcessTabState): StringsState {
    let ss = this.processStringsStates.get(ps);
    if (ss === undefined) {
      ss = {
        get stringsData() {
          return ps.processStringsData;
        },
        set stringsData(v) {
          ps.processStringsData = v;
        },
        get stringsFilterKey() {
          return ps.processStringsFilterKey;
        },
        set stringsFilterKey(v) {
          ps.processStringsFilterKey = v;
        },
        get stringsInitialFilters() {
          return ps.processStringsInitialFilters;
        },
        set stringsInitialFilters(v) {
          ps.processStringsInitialFilters = v;
        },
        get cachedDups() {
          return ps.processStringsDups;
        },
        set cachedDups(v) {
          ps.processStringsDups = v;
        },
        get cachedDupsStrings() {
          return ps.processStringsDupsStrings;
        },
        set cachedDupsStrings(v) {
          ps.processStringsDupsStrings = v;
        },
      };
      this.processStringsStates.set(ps, ss);
    }
    return ss;
  }

  // ── View ────────────────────────────────────────────────────────────────

  view(vnode: m.Vnode<SmapsExplorerPageAttrs>) {
    const {app} = vnode.attrs;

    return m(
      DetailsShell,
      {
        title: 'Smaps Explorer',
        description: this.renderHeaderDescription(),
        buttons: this.renderHeaderButtons(),
      },
      m('.pf-smaps-explorer__panel', [
        // Error banner
        this.error !== null &&
          m('.pf-smaps-explorer__error-banner', this.error),

        // Connect screen
        !this.conn.connected && this.processes === null && this.renderConnect(),

        // Connected content
        this.processes !== null && this.renderContent(app),
      ]),
    );
  }

  // ── Header ──────────────────────────────────────────────────────────────

  private renderHeaderDescription(): m.Children {
    if (!this.conn.connected) return undefined;
    const parts: m.Children[] = [];
    parts.push(`${this.processes?.length ?? 0} processes`);
    if (!this.conn.isRoot) {
      parts.push(m('span.pf-smaps-explorer__badge--warning', 'Not rooted'));
    }
    if (this.enriching && this.enrichProgress !== null) {
      parts.push(
        ` \u2014 Fetching rollups: ${this.enrichProgress.done}/${this.enrichProgress.total}`,
      );
    }
    if (this.scanningAllSmaps && this.scanAllProgress !== null) {
      parts.push(
        ` \u2014 Scanning smaps: ${this.scanAllProgress.done}/${this.scanAllProgress.total}`,
      );
    }
    if (this.smapsData.size > 0) {
      parts.push(` \u2014 ${this.smapsData.size} scanned`);
    }
    return parts;
  }

  private renderHeaderButtons(): m.Children {
    if (!this.conn.connected) return undefined;
    return [
      m(Button, {
        label: 'Refresh',
        icon: 'refresh',
        compact: true,
        onclick: () => this.refreshProcesses(),
      }),
      this.conn.isRoot &&
        !this.enriching &&
        !this.scanningAllSmaps &&
        m(Button, {
          label: 'Scan All Processes',
          icon: 'speed',
          compact: true,
          onclick: () => this.enrichAll(),
        }),
      this.conn.isRoot &&
        !this.scanningAllSmaps &&
        m(Button, {
          label: 'Scan All VMAs',
          icon: 'memory',
          compact: true,
          onclick: () => this.scanAllSmaps(),
        }),
      m(Button, {
        label: 'Disconnect',
        icon: 'link_off',
        compact: true,
        onclick: () => {
          this.conn.disconnect();
          this.processes = null;
          this.smapsData.clear();
          this.rollups.clear();
          this.s.openProcesses.clear();
          this.s.openProcessOrder.length = 0;
          this.s.activeProcessPid = null;
          this.s.openVmaMappings.clear();
          this.s.openVmaMappingOrder.length = 0;
          this.s.activeVmaMapping = null;
          this.s.topView = 0;
          this.s.processTab = TAB_PROCESSES;
          this.s.vmaTab = TAB_VMAS;
          m.redraw();
        },
      }),
    ];
  }

  // ── Connection ──────────────────────────────────────────────────────────

  private renderConnect(): m.Children {
    return m('.pf-smaps-explorer__connect', [
      m(Button, {
        label: this.connectStatus ?? 'Connect USB Device',
        icon: 'usb',
        disabled: this.connectStatus !== null,
        onclick: () => this.handleConnect(),
      }),
      m(
        'p.pf-smaps-explorer__connect-hint',
        'Enable USB debugging. Stop adb first: ',
        m('code', 'adb kill-server'),
      ),
    ]);
  }

  private async handleConnect(): Promise<void> {
    try {
      this.connectStatus = 'Connecting\u2026';
      this.error = null;
      m.redraw();
      await this.conn.connect((msg) => {
        this.connectStatus = msg;
        m.redraw();
      });
      this.connectStatus = null;
      await this.refreshProcesses();
    } catch (e) {
      this.connectStatus = null;
      this.error = e instanceof Error ? e.message : 'Connection failed';
      m.redraw();
    }
  }

  private async refreshProcesses(): Promise<void> {
    try {
      this.smapsData.clear();
      this.rollups.clear();
      for (const pid of this.s.openProcessOrder) {
        this.loadSmaps(pid);
      }
      this.processes = await this.conn.getProcessList();
      this.processes.sort((a, b) => {
        const aPin = PINNED_PROCESSES.has(a.name) ? 0 : 1;
        const bPin = PINNED_PROCESSES.has(b.name) ? 0 : 1;
        if (aPin !== bPin) return aPin - bPin;
        return a.name.localeCompare(b.name);
      });
      m.redraw();
      if (this.conn.isRoot && !this.enriching) {
        this.enrichAll();
      }
    } catch (e) {
      this.error = e instanceof Error ? e.message : 'Failed to get processes';
      m.redraw();
    }
  }

  private async enrichAll(): Promise<void> {
    if (this.processes === null || this.enriching) return;
    this.enriching = true;
    m.redraw();
    try {
      this.rollups = await this.conn.enrichProcesses(
        this.processes,
        (done, total) => {
          this.enrichProgress = {done, total};
          m.redraw();
        },
      );
    } catch {
      // Ignore enrichment failures
    } finally {
      this.enriching = false;
      this.enrichProgress = null;
      this.enrichGeneration++;
      m.redraw();
    }
    if (!this.scanningAllSmaps) {
      this.scanAllSmaps();
    }
  }

  private async scanAllSmaps(): Promise<void> {
    if (this.processes === null || this.scanningAllSmaps) return;
    this.scanningAllSmaps = true;
    const total = this.processes.length;
    let done = 0;
    this.scanAllProgress = {done, total};
    m.redraw();
    try {
      for (const p of this.processes) {
        if (this.smapsData.has(p.pid)) {
          done++;
          this.scanAllProgress = {done, total};
          m.redraw();
          continue;
        }
        try {
          const entries = await this.conn.getSmapsForPid(p.pid);
          this.smapsData.set(p.pid, aggregateSmaps(entries));
        } catch {
          // Skip processes that fail (zombie, permission, etc.)
        }
        done++;
        this.scanAllProgress = {done, total};
        m.redraw();
      }
    } finally {
      this.scanningAllSmaps = false;
      this.scanAllProgress = null;
      this.smapsScanGeneration++;
      m.redraw();
    }
  }

  // ── Main content ────────────────────────────────────────────────────────

  private renderContent(app: App): m.Children {
    return m('.pf-smaps-explorer__content', [
      this.conn.isRoot &&
        m(
          '.pf-smaps-explorer__view-selector',
          m(SegmentedButtons, {
            options: [
              {label: 'Process View', icon: 'apps'},
              {label: 'VMA View', icon: 'memory'},
            ],
            selectedOption: this.s.topView,
            onOptionSelected: (idx) => {
              this.s.topView = idx as 0 | 1;
            },
          }),
        ),
      m(
        '.pf-smaps-explorer__grid-container',
        this.s.topView === 0
          ? renderProcessView(this.ctx, app)
          : renderVmaView(this.ctx),
      ),
    ]);
  }

  // ── Navigation actions ──────────────────────────────────────────────────

  private inspectProcess(pid: number) {
    const ps = this.getProcessState(pid);
    ps.subTab = TAB_INSPECT;
    this.s.activeProcessPid = pid;
    this.s.processTab = procTabKey(pid);
    this.loadSmaps(pid);
  }

  private openMapping(ps: ProcessTabState, name: string) {
    if (!ps.openMappings.has(name)) {
      ps.openMappings.set(name, newMappingTabState());
      if (!ps.openMappingOrder.includes(name)) {
        ps.openMappingOrder.push(name);
      }
    }
    ps.activeMapping = name;
    ps.subTab = mapTabKey(name);
  }

  private openVmaProcesses(name: string) {
    if (!this.s.openVmaMappings.has(name)) {
      this.s.openVmaMappings.set(name, newVmaMappingTabState());
      if (!this.s.openVmaMappingOrder.includes(name)) {
        this.s.openVmaMappingOrder.push(name);
      }
    }
    this.s.activeVmaMapping = name;
    this.s.vmaTab = vmapTabKey(name);
  }

  private openVmaProcDetail(vs: VmaMappingTabState, pid: number) {
    if (!vs.openProcs.has(pid)) {
      vs.openProcs.set(pid, newMappingTabState());
      if (!vs.openProcOrder.includes(pid)) {
        vs.openProcOrder.push(pid);
      }
    }
    vs.activeProc = pid;
    vs.subTab = procTabKey(pid);
    this.loadSmaps(pid);
  }

  // ── Data loading ────────────────────────────────────────────────────────

  private async loadSmaps(pid: number): Promise<void> {
    if (this.loadingPid === pid || this.smapsData.has(pid)) return;
    this.loadingPid = pid;
    m.redraw();
    try {
      const entries = await this.conn.getSmapsForPid(pid);
      this.smapsData.set(pid, aggregateSmaps(entries));
    } catch (e) {
      this.error = e instanceof Error ? e.message : 'Failed to load smaps';
    } finally {
      if (this.loadingPid === pid) this.loadingPid = null;
      m.redraw();
    }
  }

  private async scanSingleVma(
    pid: number,
    ms: MappingTabState,
    addrStart: string,
    addrEnd: string,
    perms: string,
  ): Promise<void> {
    if (!this.conn.isRoot || perms[0] !== 'r') {
      this.error = perms[0] !== 'r' ? 'VMA is not readable' : 'Root required';
      m.redraw();
      return;
    }

    const rawAgg = this.smapsData.get(pid);
    if (rawAgg === undefined) return;
    let targetEntry: SmapsEntry | undefined;
    for (const g of rawAgg) {
      for (const e of g.entries) {
        if (e.addrStart === addrStart && e.addrEnd === addrEnd) {
          targetEntry = e;
          break;
        }
      }
      if (targetEntry) break;
    }
    if (targetEntry === undefined) return;

    const entryName = targetEntry.name || '[anonymous]';
    const liveData: ProcessStringsResult = {
      pid,
      processName: `${addrStart}-${addrEnd} ${entryName}`,
      regions: [
        {
          addrStart: targetEntry.addrStart,
          addrEnd: targetEntry.addrEnd,
          perms: targetEntry.perms,
          name: targetEntry.name,
          sizeKb: targetEntry.sizeKb,
          stringCount: 0,
        },
      ],
      strings: [],
      scanning: true,
      scannedVmas: 0,
      totalVmas: 1,
    };
    ms.stringsData = liveData;
    ms.subTab = TAB_STRINGS_DUPS;
    m.redraw();

    try {
      await this.conn.grepVmaStrings(
        pid,
        [targetEntry],
        (newStrings, regions, completed, total) => {
          for (const s of newStrings) liveData.strings.push(s);
          liveData.regions = regions;
          liveData.scannedVmas = completed;
          liveData.totalVmas = total;
          ms.stringsData = {...liveData, strings: [...liveData.strings]};
          m.redraw();
        },
      );
      liveData.scanning = false;
      ms.stringsData = {...liveData, strings: [...liveData.strings]};
      m.redraw();
    } catch (e) {
      this.error = e instanceof Error ? e.message : 'String scan failed';
      m.redraw();
    }
  }

  private async captureHeap(
    pid: number,
    name: string,
    app: App,
  ): Promise<void> {
    try {
      this.error = null;
      const data = await this.conn.captureHeapDump(pid, (status) => {
        this.error = status;
        m.redraw();
      });
      this.error = null;
      m.redraw();
      await app.openTraceFromBuffer({
        buffer: data.buffer as ArrayBuffer,
        title: `${name} (${pid})`,
        fileName: `${name}_${pid}.hprof`,
      });
    } catch (e) {
      this.error = e instanceof Error ? e.message : 'Heap dump failed';
      m.redraw();
    }
  }

  private async startStringsScan(
    pid: number,
    processName: string,
    ps: ProcessTabState,
  ): Promise<void> {
    if (!this.conn.isRoot) return;
    try {
      let aggregated = this.smapsData.get(pid);
      if (aggregated === undefined) {
        this.error = 'Fetching smaps\u2026';
        m.redraw();
        const entries = await this.conn.getSmapsForPid(pid);
        aggregated = aggregateSmaps(entries);
        this.smapsData.set(pid, aggregated);
        this.error = null;
      }

      const allEntries = aggregated.flatMap((a) => a.entries);
      const readable = allEntries.filter((e) => e.perms[0] === 'r');

      const liveData: ProcessStringsResult = {
        pid,
        processName,
        regions: readable.map((e) => ({
          addrStart: e.addrStart,
          addrEnd: e.addrEnd,
          perms: e.perms,
          name: e.name,
          sizeKb: e.sizeKb,
          stringCount: 0,
        })),
        strings: [],
        scanning: true,
        scannedVmas: 0,
        totalVmas: readable.length,
      };
      ps.processStringsData = liveData;
      ps.activeMapping = null;
      ps.subTab = TAB_STRINGS_DUPS;
      m.redraw();

      await this.conn.grepVmaStrings(
        pid,
        allEntries,
        (newStrings, regions, completed, total) => {
          for (const s of newStrings) liveData.strings.push(s);
          liveData.regions = regions;
          liveData.scannedVmas = completed;
          liveData.totalVmas = total;
          ps.processStringsData = {
            ...liveData,
            strings: [...liveData.strings],
          };
          m.redraw();
        },
      );

      liveData.scanning = false;
      ps.processStringsData = {
        ...liveData,
        strings: [...liveData.strings],
      };
      m.redraw();
    } catch (e) {
      this.error = e instanceof Error ? e.message : 'String scan failed';
      m.redraw();
    }
  }
}
