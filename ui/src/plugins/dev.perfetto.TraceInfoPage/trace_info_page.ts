// Copyright (C) 2025 The Android Open Source Project
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
import type {Trace} from '../../public/trace';
import {EmptyState} from '../../widgets/empty_state';
import {Tabs, type TabsTab} from '../../widgets/tabs';
import type {TabKey} from './utils';
import {isValidTabKey} from './utils';
import {OverviewTab} from './tabs/overview';
import {type OverviewData, loadOverviewData} from './tabs/overview_data';
import {ConfigTab, type ConfigData, loadConfigData} from './tabs/config';
import {
  AndroidTab,
  type AndroidData,
  loadAndroidData,
  hasAndroidData,
} from './tabs/android';
import {
  MachinesTab,
  type MachinesData,
  loadMachinesData,
} from './tabs/machines';
import {TracesTab, type TracesData, loadTracesData} from './tabs/traces';
import {
  ImportErrorsTab,
  type ImportErrorsData,
  loadImportErrorsData,
} from './tabs/import_errors';
import {
  DataLossesTab,
  type DataLossesData,
  loadDataLossesData,
} from './tabs/data_losses';
import {
  TraceErrorsTab,
  type TraceErrorsData,
  loadTraceErrorsData,
} from './tabs/trace_errors';
import {NoticesTab, type NoticesData, loadNoticesData} from './tabs/notices';
import {
  UiLoadingErrorsTab,
  type UiLoadingErrorsData,
} from './tabs/ui_loading_errors';
import {StatsTab, type StatsData, loadStatsData} from './tabs/stats';
import {
  TraceDoctorTab,
  type Diagnostic,
  loadTraceDiagnostics,
} from './diagnostics';
import {
  MetadataTab,
  type MetadataData,
  loadMetadataData,
  hasMetadataData,
} from './tabs/metadata';

export interface TraceInfoPageAttrs {
  readonly trace: Trace;
  readonly subpage: string | undefined;
}

interface AllTabData {
  overview: OverviewData;
  diagnostics: ReadonlyArray<Diagnostic>;
  config: ConfigData;
  android: AndroidData;
  machines: MachinesData;
  traces: TracesData;
  metadata: MetadataData;
  importErrors: ImportErrorsData;
  traceErrors: TraceErrorsData;
  dataLosses: DataLossesData;
  notices: NoticesData;
  uiLoadingErrors: UiLoadingErrorsData;
  stats: StatsData;
}

export class TraceInfoPage implements m.ClassComponent<TraceInfoPageAttrs> {
  // All tab data
  private tabData?: AllTabData;
  private currentTab: TabKey = 'overview';
  private lastSubpage?: string;

  oninit({attrs}: m.CVnode<TraceInfoPageAttrs>) {
    this.loadAllData(attrs.trace);
  }

  view({attrs}: m.CVnode<TraceInfoPageAttrs>) {
    if (attrs.subpage !== this.lastSubpage) {
      this.lastSubpage = attrs.subpage;
      this.currentTab = getTab(attrs.subpage);
    }
    return m(
      '.pf-trace-info-page',
      m(
        '.pf-trace-info-page__inner',
        m(
          '.pf-trace-info-page__header',
          m('h1.pf-trace-info-page__header-title', 'Overview'),
          m(
            '.pf-trace-info-page__subtitle',
            'High-level summary of trace health, metrics, and system information',
          ),
        ),
        m(Tabs, {
          variant: 'underline',
          activeTabKey: this.currentTab,
          onTabChange: (key: string) => {
            this.currentTab = isValidTabKey(key) ? key : 'overview';
          },
          tabs: this.getTabs(attrs.trace),
        }),
      ),
    );
  }

  private async loadAllData(trace: Trace): Promise<void> {
    const engine = trace.engine;
    this.tabData = {
      overview: await loadOverviewData(trace),
      diagnostics: await loadTraceDiagnostics(engine),
      config: await loadConfigData(engine),
      android: await loadAndroidData(engine),
      machines: await loadMachinesData(engine),
      traces: await loadTracesData(engine),
      metadata: await loadMetadataData(engine),
      importErrors: await loadImportErrorsData(engine),
      traceErrors: await loadTraceErrorsData(engine),
      dataLosses: await loadDataLossesData(engine),
      notices: await loadNoticesData(engine),
      uiLoadingErrors: {errors: trace.loadingErrors},
      stats: await loadStatsData(engine),
    };
    m.redraw();
  }

  // The page's tabs: a tab exists only while its data does, except overview
  // and statistics, which are always present. Each tab's content lives next
  // to its metadata.
  private getTabs(trace: Trace): TabsTab[] {
    const data = this.tabData;
    if (data === undefined) {
      // Still loading: show the two unconditional tabs with a loading state.
      // A fresh vnode per tab, as the same vnode cannot be rendered in two
      // places.
      const renderLoading = () =>
        m(EmptyState, {
          icon: 'hourglass_empty',
          title: 'Loading trace info...',
        });
      return [
        {key: 'overview', title: 'Overview', content: renderLoading()},
        {key: 'stats', title: 'Statistics', content: renderLoading()},
      ];
    }
    const tabs: TabsTab[] = [
      {
        key: 'overview',
        title: 'Overview',
        content: m(OverviewTab, {
          trace,
          data: data.overview,
          diagnostics: data.diagnostics,
          onTabChange: (key: TabKey) => {
            this.currentTab = key;
          },
        }),
      },
    ];
    if (data.config.configs.length > 0) {
      tabs.push({
        key: 'config',
        title: 'Trace Config',
        content: m(ConfigTab, {data: data.config}),
      });
    }
    if (data.overview.importErrors > 0) {
      tabs.push({
        key: 'import_errors',
        title: 'Import Errors',
        content: m(ImportErrorsTab, {data: data.importErrors}),
      });
    }
    if (data.traceErrors.errors.length > 0) {
      tabs.push({
        key: 'trace_errors',
        title: 'Trace Errors',
        content: m(TraceErrorsTab, {data: data.traceErrors}),
      });
    }
    if (data.diagnostics.length > 0) {
      tabs.push({
        key: 'trace_doctor',
        title: 'Trace Doctor',
        content: m(TraceDoctorTab, {
          diagnostics: data.diagnostics,
          isMultiTrace: data.overview.traceCount > 1,
        }),
      });
    }
    if (data.overview.dataLosses > 0) {
      tabs.push({
        key: 'data_losses',
        title: 'Data Losses',
        content: m(DataLossesTab, {data: data.dataLosses}),
      });
    }
    if (data.notices.categories.length > 0) {
      tabs.push({
        key: 'notices',
        title: 'Notices',
        content: m(NoticesTab, {data: data.notices}),
      });
    }
    if (data.overview.uiLoadingErrorCount > 0) {
      tabs.push({
        key: 'ui_loading_errors',
        title: 'UI Loading Errors',
        content: m(UiLoadingErrorsTab, {data: data.uiLoadingErrors}),
      });
    }
    if (hasAndroidData(data.android)) {
      tabs.push({
        key: 'android',
        title: 'Android',
        content: m(AndroidTab, {data: data.android}),
      });
    }
    if (data.overview.traceCount > 1) {
      tabs.push({
        key: 'traces',
        title: 'Traces',
        content: m(TracesTab, {data: data.traces}),
      });
    }
    if (data.machines.machineCount > 1) {
      tabs.push({
        key: 'machines',
        title: 'Machines',
        content: m(MachinesTab, {data: data.machines}),
      });
    }
    if (hasMetadataData(data.metadata)) {
      tabs.push({
        key: 'metadata',
        title: 'Metadata',
        content: m(MetadataTab, {data: data.metadata}),
      });
    }
    tabs.push({
      key: 'stats',
      title: 'Statistics',
      content: m(StatsTab, {data: data.stats}),
    });
    return tabs;
  }
}

function getTab(subpage: string | undefined): TabKey {
  if (!subpage) {
    return 'overview';
  }
  const res = subpage.substring(1);
  return isValidTabKey(res) ? res : 'overview';
}
