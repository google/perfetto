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
import {TabStrip} from '../../widgets/tab_strip';
import {EmptyState} from '../../widgets/empty_state';
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
        m(TabStrip, {variant: 'underline'}, this.getTabs()),
        this.renderCurrentTab(attrs.trace, this.currentTab),
      ),
    );
  }

  private renderCurrentTab(trace: Trace, currentTab: TabKey): m.Children {
    if (!this.tabData) {
      return m(EmptyState, {
        icon: 'hourglass_empty',
        title: 'Loading trace info...',
      });
    }
    switch (currentTab) {
      case 'overview':
        return m(OverviewTab, {
          trace,
          data: this.tabData.overview,
          diagnostics: this.tabData.diagnostics,
          onTabChange: (key: TabKey) => {
            this.currentTab = key;
          },
        });
      case 'trace_doctor':
        return m(TraceDoctorTab, {
          diagnostics: this.tabData.diagnostics,
          isMultiTrace: this.tabData.overview.traceCount > 1,
        });
      case 'config':
        return m(ConfigTab, {
          data: this.tabData.config,
        });
      case 'android':
        return m(AndroidTab, {
          data: this.tabData.android,
        });
      case 'traces':
        return m(TracesTab, {
          data: this.tabData.traces,
        });
      case 'machines':
        return m(MachinesTab, {
          data: this.tabData.machines,
        });
      case 'metadata':
        return m(MetadataTab, {
          data: this.tabData.metadata,
        });
      case 'import_errors':
        return m(ImportErrorsTab, {
          data: this.tabData.importErrors,
        });
      case 'trace_errors':
        return m(TraceErrorsTab, {
          data: this.tabData.traceErrors,
        });
      case 'data_losses':
        return m(DataLossesTab, {
          data: this.tabData.dataLosses,
        });
      case 'notices':
        return m(NoticesTab, {
          data: this.tabData.notices,
        });
      case 'ui_loading_errors':
        return m(UiLoadingErrorsTab, {
          data: this.tabData.uiLoadingErrors,
        });
      case 'stats':
        return m(StatsTab, {
          data: this.tabData.stats,
        });
    }
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

  private getTabs(): m.Children {
    const propsForTab = (key: TabKey) => {
      return {
        active: this.currentTab === key,
        onclick: () => (this.currentTab = key),
      };
    };

    return [
      m(TabStrip.Tab, propsForTab('overview'), 'Overview'),
      (this.tabData?.config?.configs?.length ?? 0) > 0 &&
        m(TabStrip.Tab, propsForTab('config'), 'Trace Config'),
      (this.tabData?.overview?.importErrors ?? 0) > 0 &&
        m(TabStrip.Tab, propsForTab('import_errors'), 'Import Errors'),
      (this.tabData?.traceErrors?.errors?.length ?? 0) > 0 &&
        m(TabStrip.Tab, propsForTab('trace_errors'), 'Trace Errors'),
      (this.tabData?.diagnostics?.length ?? 0) > 0 &&
        m(TabStrip.Tab, propsForTab('trace_doctor'), 'Trace Doctor'),
      (this.tabData?.overview?.dataLosses ?? 0) > 0 &&
        m(TabStrip.Tab, propsForTab('data_losses'), 'Data Losses'),
      (this.tabData?.notices?.categories?.length ?? 0) > 0 &&
        m(TabStrip.Tab, propsForTab('notices'), 'Notices'),
      (this.tabData?.overview?.uiLoadingErrorCount ?? 0) > 0 &&
        m(TabStrip.Tab, propsForTab('ui_loading_errors'), 'UI Loading Errors'),
      hasAndroidData(this.tabData?.android) &&
        m(TabStrip.Tab, propsForTab('android'), 'Android'),
      (this.tabData?.overview?.traceCount ?? 0) > 1 &&
        m(TabStrip.Tab, propsForTab('traces'), 'Traces'),
      (this.tabData?.machines?.machineCount ?? 0) > 1 &&
        m(TabStrip.Tab, propsForTab('machines'), 'Machines'),
      hasMetadataData(this.tabData?.metadata) &&
        m(TabStrip.Tab, propsForTab('metadata'), 'Metadata'),
      m(TabStrip.Tab, propsForTab('stats'), 'Statistics'),
    ];
  }
}

function getTab(subpage: string | undefined): TabKey {
  if (!subpage) {
    return 'overview';
  }
  const res = subpage.substring(1);
  return isValidTabKey(res) ? res : 'overview';
}
