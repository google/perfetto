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
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import type {Engine} from '../../trace_processor/engine';
import {
  KernelMetricsSection,
  type KernelGroup,
  fetchRawKernelMetricGroups,
  buildKernelMetricDataFromGroup,
} from './details';
import type {TrackEventSelection} from '../../public/selection';
import {renderToolbar} from './toolbar';
import type {InfoTab} from './toolbar';
import type {
  KernelLaunchOption,
  KernelMetricData,
  ToolbarInfo,
} from './details';
import {KernelSummarySection, fetchKernelSummaryRows} from './summary';
import type {SummaryRow} from './summary';
import {registerSpeedOfLightSection} from './section/speed_of_light';
import {registerLaunchStatisticsSection} from './section/launch_statistics';
import {registerOccupancySection} from './section/occupancy';
import {registerWorkloadAnalysisSection} from './section/workload_analysis';
import {cudaTerminology} from './terminology/cuda';
import {openclTerminology} from './terminology/opencl';
import {TerminologyRegistry} from './terminology';
import {SectionRegistry} from './section';
import {
  type PerformanceAnalysisResult,
  type AnalysisCache,
  type AnalysisProvider,
  AnalysisProviderHolder,
} from './analysis';
import type {Tab} from '../../public/tab';
import {maybeUndefined} from '../../base/utils';
import {AsyncMemo} from '../../base/async_memo';

export interface GpuComputeContext {
  humanizeMetrics: boolean;
  activeInfoTab: InfoTab;
  terminologyId: string;
  readonly terminologyRegistry: TerminologyRegistry;
  readonly sectionRegistry: SectionRegistry;
  readonly analysisProviderHolder: AnalysisProviderHolder;
}

class ComputeTab implements Tab {
  private readonly ctx: GpuComputeContext;
  private readonly knownKernelIds = new Set<number>();

  // Selection-driven metric fetching via QuerySlot. Selection changes
  // trigger mithril redraws; render() reads the current selection and
  // polls the QuerySlot which handles deduplication, background
  // fetching, and race-condition prevention.
  private readonly selectionSlot = new AsyncMemo<KernelGroup[]>();
  private readonly baselineSlot = new AsyncMemo<KernelGroup[]>();

  private selectedKernelId?: number;
  private baselineKernelId?: number;
  private prevTimelineSelectionId?: number;

  // Cache for storing analysis results by sliceId
  private readonly kernelAnalysisCache = new Map<
    number,
    PerformanceAnalysisResult
  >();
  // Cache for storing section analysis results by sliceId and section name
  private readonly sectionAnalysisCache = new Map<
    string,
    PerformanceAnalysisResult
  >();

  // Helper to create cache key for section analysis
  private getSectionCacheKey(sliceId: number, sectionName: string): string {
    return `${sliceId}:${sectionName}`;
  }

  // Analysis cache interface passed to the KernelAnalysisSection component
  private readonly analysisCache: AnalysisCache = {
    getKernelAnalysis: (sliceId: number) =>
      this.kernelAnalysisCache.get(sliceId),
    setKernelAnalysis: (sliceId: number, result: PerformanceAnalysisResult) => {
      this.kernelAnalysisCache.set(sliceId, result);
    },
    getSectionAnalysis: (sliceId: number, sectionName: string) =>
      this.sectionAnalysisCache.get(
        this.getSectionCacheKey(sliceId, sectionName),
      ),
    setSectionAnalysis: (
      sliceId: number,
      sectionName: string,
      result: PerformanceAnalysisResult,
    ) => {
      this.sectionAnalysisCache.set(
        this.getSectionCacheKey(sliceId, sectionName),
        result,
      );
    },
  };

  constructor(
    private readonly engine: Engine,
    private readonly trace: Trace,
    private readonly tabUri: string,
    terminologyRegistry: TerminologyRegistry,
    sectionRegistry: SectionRegistry,
    analysisProviderHolder: AnalysisProviderHolder,
    private readonly summaryRows: readonly SummaryRow[],
    private readonly options: readonly KernelLaunchOption[],
  ) {
    this.ctx = {
      humanizeMetrics: true,
      activeInfoTab: 'summary',
      terminologyId: 'cuda',
      terminologyRegistry,
      sectionRegistry,
      analysisProviderHolder,
    };
    this.knownKernelIds = new Set(options.map((o) => o.id));

    // Initially select the first kernel (if we have one)
    this.selectedKernelId = maybeUndefined(this.options[0])?.id;
  }

  // Fetch and process all kernel-related performance metric data from the trace
  // If a slice is selected, we pass sliceId so the component shows only the
  // selected kernel compute metrics if they are available
  render(): m.Children {
    this.maybeSyncTimelineSelection();

    const {toolbar: toolbarInfo, data: selectionData} = this.getSelectionData();
    const {toolbar: baselineToolbarInfo, data: baselineData} =
      this.getBaselineData();

    return m('', [
      renderToolbar({
        ctx: this.ctx,
        options: this.options,
        sliceId: this.selectedKernelId,
        onChange: (id, suppress) => this.setSliceId(id, suppress),
        toolbarInfo,
        baselineId: this.baselineKernelId,
        baselineInfo: baselineToolbarInfo,
        baselineEnabled: this.baselineKernelId !== undefined,
        onToggleBaseline: (enabled: boolean) => this.setBaselineId(enabled),
      }),
      this.renderBody(selectionData, baselineData),
    ]);
  }

  getTitle() {
    return 'GPU Compute';
  }

  // Fetch and process all kernel-related performance metric data from the trace
  private renderBody(
    selectionData?: KernelMetricData[],
    baselineData?: KernelMetricData,
  ): m.Children {
    if (this.ctx.activeInfoTab === 'summary') {
      return m(KernelSummarySection, {
        ctx: this.ctx,
        engine: this.engine,
        sliceId: this.selectedKernelId,
        openSliceInDetail: (id: number) => this.setSliceId(id),
        prefetchedRows: this.summaryRows,
      });
    }

    if (selectionData === undefined) {
      return null;
    }

    if (this.ctx.activeInfoTab === 'analysis') {
      const provider = this.ctx.analysisProviderHolder.get();
      if (provider) {
        return provider.renderAnalysisTab({
          engine: this.engine,
          sliceId: this.selectedKernelId,
          analysisCache: this.analysisCache,
        });
      }
      return m('.pf-gpu-compute__pad', 'Analysis plugin not enabled.');
    }

    return m(KernelMetricsSection, {
      ctx: this.ctx,
      engine: this.engine,
      sliceId: this.selectedKernelId,
      data: selectionData,
      baseline: baselineData,
      analysisCache: this.analysisCache,
    });
  }

  // Updates which kernel is shown from the dropdown or summary section.
  private setSliceId(sliceId: number, suppressAutoDetails = false) {
    this.selectedKernelId = sliceId;
    if (!suppressAutoDetails) {
      this.ctx.activeInfoTab = 'details';
    }
  }

  private setBaselineId(enabled: boolean) {
    // If not enabled, clear baseline state
    if (!enabled) {
      this.baselineKernelId = undefined;
      return;
    }

    // Setting the baseline id to the current selection, otherwise fallback to the first kernel launch metrics
    const id = this.selectedKernelId ?? this.options[0]?.id;
    if (id === undefined) return;
    this.baselineKernelId = id;
  }

  private getSelectionData(): {
    toolbar?: ToolbarInfo;
    data?: KernelMetricData[];
  } {
    const sliceId = this.selectedKernelId;
    if (sliceId === undefined) return {};

    const selectionResult = this.selectionSlot.use({
      key: {sliceId},
      retainOn: ['sliceId'],
      compute: async () => {
        return fetchRawKernelMetricGroups(this.ctx, this.engine, sliceId);
      },
    });

    const groups = selectionResult.data;
    if (!groups) return {};

    const data = groups.map((g) => buildKernelMetricDataFromGroup(this.ctx, g));
    return {
      toolbar: data[0]?.toolbar,
      data,
    };
  }

  private getBaselineData(): {
    toolbar?: ToolbarInfo;
    data?: KernelMetricData;
  } {
    if (this.baselineKernelId === undefined) {
      return {};
    }

    const baselineResult = this.baselineSlot.use({
      key: {sliceId: this.baselineKernelId},
      retainOn: ['sliceId'],
      compute: async () => {
        return fetchRawKernelMetricGroups(
          this.ctx,
          this.engine,
          this.baselineKernelId!,
        );
      },
    });

    const groups = baselineResult.data;
    if (!groups || groups.length === 0) return {};

    const data = buildKernelMetricDataFromGroup(this.ctx, groups[0]);
    return {
      toolbar: data.toolbar,
      data,
    };
  }

  private maybeSyncTimelineSelection(): void {
    const sel = this.trace.selection.selection;
    const selSliceId =
      sel.kind === 'track_event'
        ? (sel as TrackEventSelection).eventId
        : undefined;

    if (
      selSliceId !== undefined &&
      selSliceId !== this.prevTimelineSelectionId
    ) {
      this.prevTimelineSelectionId = selSliceId;
      if (this.knownKernelIds.has(selSliceId)) {
        this.selectedKernelId = selSliceId;
        this.ctx.activeInfoTab = 'details';
        this.trace.tabs.showTab(this.tabUri);
      }
    }
  }
}

// Plugin entry point, registers the tab and menu command
export default class GpuComputePlugin implements PerfettoPlugin {
  static readonly id = 'com.meta.GpuCompute';
  static readonly description =
    'Analyzes GPU compute kernel performance using hardware counters ' +
    'and launch parameters found in the trace.';

  private readonly terminologyRegistry = new TerminologyRegistry();
  private readonly sectionRegistry = new SectionRegistry();
  private readonly analysisProviderHolder = new AnalysisProviderHolder();

  constructor() {
    this.terminologyRegistry.register('cuda', cudaTerminology);
    this.terminologyRegistry.register('opencl', openclTerminology);
    registerSpeedOfLightSection(this.sectionRegistry);
    registerLaunchStatisticsSection(this.sectionRegistry);
    registerOccupancySection(this.sectionRegistry);
    registerWorkloadAnalysisSection(this.sectionRegistry);
  }

  registerAnalysisProvider(provider: AnalysisProvider): void {
    this.analysisProviderHolder.register(provider);
  }

  getSectionRegistry(): SectionRegistry {
    return this.sectionRegistry;
  }

  getContext(): GpuComputeContext {
    return {
      humanizeMetrics: true,
      activeInfoTab: 'details',
      terminologyId: 'cuda',
      terminologyRegistry: this.terminologyRegistry,
      sectionRegistry: this.sectionRegistry,
      analysisProviderHolder: this.analysisProviderHolder,
    };
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    const tabUri = `${GpuComputePlugin.id}#Compute`;

    const rows = await this.fetchSummaryRows(trace);
    const options = rows.map((r) => ({id: r.id, label: r.demangledName}));

    if (options.length === 0) {
      // No kernels - don't show the tab
      return;
    }

    const content = new ComputeTab(
      trace.engine,
      trace,
      tabUri,
      this.terminologyRegistry,
      this.sectionRegistry,
      this.analysisProviderHolder,
      rows,
      options,
    );

    trace.commands.registerCommand({
      id: `${GpuComputePlugin.id}#ShowComputeTab`,
      name: 'Show Compute Tab',
      callback: () => trace.tabs.showTab(tabUri),
    });

    trace.tabs.registerTab({
      isEphemeral: false,
      uri: tabUri,
      content: content,
    });

    if (options.length > 0) {
      trace.tabs.showTab(tabUri);
    }
  }

  private async fetchSummaryRows(trace: Trace) {
    try {
      const rows = await fetchKernelSummaryRows(
        this.getContext(),
        trace.engine,
      );
      rows.sort((a, b) => a.id - b.id);
      return rows;
    } catch (e) {
      console.warn('GpuCompute: failed to fetch kernel launch list:', e);
      return [];
    }
  }
}
