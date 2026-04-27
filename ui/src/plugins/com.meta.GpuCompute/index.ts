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
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {Engine} from '../../trace_processor/engine';
import {KernelMetricsSection, fetchSelectedKernelMetricData} from './details';
import {TrackEventSelection} from '../../public/selection';
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
  PerformanceAnalysisResult,
  AnalysisCache,
  AnalysisProvider,
  AnalysisProviderHolder,
} from './analysis';
import {SerialTaskQueue, QuerySlot} from '../../base/query_slot';

export interface GpuComputeContext {
  humanizeMetrics: boolean;
  activeInfoTab: InfoTab;
  terminologyId: string;
  readonly terminologyRegistry: TerminologyRegistry;
  readonly sectionRegistry: SectionRegistry;
  readonly analysisProviderHolder: AnalysisProviderHolder;
}

class Compute {
  readonly ctx: GpuComputeContext;

  constructor(
    private readonly engine: Engine,
    private readonly trace: Trace,
    private readonly tabUri: string,
    terminologyRegistry: TerminologyRegistry,
    sectionRegistry: SectionRegistry,
    analysisProviderHolder: AnalysisProviderHolder,
  ) {
    this.ctx = {
      humanizeMetrics: true,
      activeInfoTab: 'summary',
      terminologyId: 'cuda',
      terminologyRegistry,
      sectionRegistry,
      analysisProviderHolder,
    };
  }

  private sliceId: number | undefined = -1;
  private options: KernelLaunchOption[] = [];
  private summaryRows: SummaryRow[] = [];
  private knownKernelIds = new Set<number>();

  // Selection-driven metric fetching via QuerySlot. Selection changes
  // trigger mithril redraws; render() reads the current selection and
  // polls the QuerySlot which handles deduplication, background
  // fetching, and race-condition prevention.
  private readonly taskQueue = new SerialTaskQueue();
  private readonly selectionSlot = new QuerySlot<{
    hasMetrics: boolean;
    toolbar?: ToolbarInfo;
  }>(this.taskQueue);
  private hadSelection = false;
  private appliedSelectionSliceId: number | undefined;

  private baselineSliceId: number | undefined = undefined;
  private baselineToolbarInfo?: ToolbarInfo;
  private baselineData?: KernelMetricData;

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

  public getTitle() {
    return 'GPU Compute';
  }

  // Updates which kernel is shown from the dropdown. Fetches toolbar
  // metrics for the selected kernel and switches to the appropriate tab.
  public async setSliceId(sliceId: number, suppressAutoDetails = false) {
    const firstId = this.options[0]?.id ?? -1;
    this.sliceId = sliceId !== -1 ? sliceId : firstId;
    const requestedSliceId = this.sliceId;

    if (this.sliceId === -1) {
      this.setToolbarInfo(undefined);
      this.ctx.activeInfoTab = 'summary';
      return;
    }

    try {
      const data = await fetchSelectedKernelMetricData(
        this.ctx,
        this.engine,
        this.sliceId,
      );
      // Guard against stale async completions: if the sliceId changed
      // while the query was in flight, discard the result.
      if (this.sliceId !== requestedSliceId) return;
      const hasMetrics = Array.isArray(data) && data.length > 0;
      this.setToolbarInfo(hasMetrics ? data[0].toolbar : undefined);
      if (!hasMetrics) {
        this.sliceId = firstId;
        this.ctx.activeInfoTab = 'summary';
      } else if (!suppressAutoDetails) {
        this.ctx.activeInfoTab = 'details';
      }
    } catch (e) {
      console.warn('GpuCompute: failed to fetch toolbar metrics:', e);
      this.setToolbarInfo(undefined);
    }
  }

  private async setBaselineId(useCurrent: boolean) {
    // If not using current, clear baseline state
    if (!useCurrent) {
      this.baselineSliceId = undefined;
      this.baselineToolbarInfo = undefined;
      this.baselineData = undefined;
      return;
    }

    // Setting the baseline id to the current selection, otherwise fallback to the first kernel launch metrics
    const id =
      this.sliceId != null && this.sliceId !== -1
        ? this.sliceId
        : this.options[0]?.id;
    if (id == null) return;
    this.baselineSliceId = id;

    // Fetching baseline metrics to populate the toolbar info with baselineData
    try {
      const data = await fetchSelectedKernelMetricData(
        this.ctx,
        this.engine,
        id,
      );
      this.baselineToolbarInfo = data?.[0]?.toolbar;
      this.baselineData = data?.[0];
    } catch {
      this.baselineToolbarInfo = undefined;
      this.baselineData = undefined;
    }
  }

  private async refreshBaseline() {
    if (this.baselineSliceId == null) return;

    try {
      const data = await fetchSelectedKernelMetricData(
        this.ctx,
        this.engine,
        this.baselineSliceId,
      );
      this.baselineToolbarInfo = data?.[0]?.toolbar;
      this.baselineData = data?.[0];
    } catch {
      this.baselineToolbarInfo = undefined;
      this.baselineData = undefined;
    }
  }

  // Updates the "Results" dropdown with new launch options
  public setOptions(opts: KernelLaunchOption[]) {
    this.options = opts;
    this.knownKernelIds = new Set(opts.map((o) => o.id));
  }

  public setSummaryRows(rows: SummaryRow[]) {
    this.summaryRows = rows;
  }

  // Populating the toolbar with the correct kernel's launch info
  private toolbarInfo?: ToolbarInfo;
  public setToolbarInfo(info?: ToolbarInfo) {
    this.toolbarInfo = info;
  }

  // Used to re-fetch the same items so both toolbar + tables reflect new mode
  private async refresh() {
    if (this.sliceId != null && this.sliceId !== -1) {
      await this.setSliceId(this.sliceId);
    }

    // If there's an active baseline, re-fetch it for the same sliceId (don't overwrite it with current!)
    if (this.baselineSliceId != null) {
      await this.refreshBaseline();
    }
  }

  // Fetch and process all kernel-related performance metric data from the trace
  // If a slice is selected, we pass sliceId so the component shows only the selected kernel compute metrics if they are available
  render(): m.Children {
    const sel = this.trace.selection.selection;
    const selSliceId =
      sel.kind === 'track_event'
        ? (sel as TrackEventSelection).eventId
        : undefined;

    if (selSliceId !== undefined) {
      this.hadSelection = true;
      const id = selSliceId;
      const isKnownKernel = this.knownKernelIds.has(id);

      // Show the tab immediately for known kernels so the user doesn't
      // see a flicker to the "Current Selection" tab while the async
      // metric query is in flight.
      if (isKnownKernel && this.appliedSelectionSliceId !== selSliceId) {
        this.sliceId = selSliceId;
        this.ctx.activeInfoTab = 'details';
        this.trace.tabs.showTab(this.tabUri);
      }

      const result = this.selectionSlot.use({
        key: {sliceId: id},
        queryFn: async () => {
          const data = await fetchSelectedKernelMetricData(
            this.ctx,
            this.engine,
            id,
          );
          const hasMetrics = Array.isArray(data) && data.length > 0;
          return {
            hasMetrics,
            toolbar: hasMetrics ? data[0].toolbar : undefined,
          };
        },
      });

      if (result.data && this.appliedSelectionSliceId !== selSliceId) {
        this.appliedSelectionSliceId = selSliceId;
        if (result.data.hasMetrics) {
          this.sliceId = selSliceId;
          this.setToolbarInfo(result.data.toolbar);
        } else if (!isKnownKernel) {
          this.sliceId = this.options[0]?.id ?? -1;
          this.setToolbarInfo(undefined);
          this.ctx.activeInfoTab = 'summary';
        }
      }
    } else if (this.hadSelection) {
      this.hadSelection = false;
      this.appliedSelectionSliceId = undefined;
    }

    const toolbar = renderToolbar({
      ctx: this.ctx,
      options: this.options,
      sliceId: this.sliceId ?? undefined,
      onChange: (id, suppress) => this.setSliceId(id ?? -1, suppress),
      toolbarInfo: this.toolbarInfo,
      baselineId: this.baselineSliceId,
      baselineInfo: this.baselineToolbarInfo,
      baselineEnabled: this.baselineSliceId != null,
      onHumanizeChanged: () => this.refresh(),
      onToggleBaseline: (enabled: boolean) => this.setBaselineId(enabled),
      onTerminologyChanged: () => this.refresh(),
    });

    const effectiveSliceId = this.sliceId ?? -1;
    let body: m.Children;

    if (this.ctx.activeInfoTab === 'summary') {
      body = m(KernelSummarySection, {
        ctx: this.ctx,
        engine: this.engine,
        sliceId: effectiveSliceId,
        openSliceInDetail: (id: number) => this.setSliceId(id),
        prefetchedRows: this.summaryRows,
      });
    } else if (this.ctx.activeInfoTab === 'analysis') {
      const provider = this.ctx.analysisProviderHolder.get();
      if (provider) {
        body = provider.renderAnalysisTab({
          engine: this.engine,
          sliceId: effectiveSliceId,
          analysisCache: this.analysisCache,
        });
      } else {
        body = m('.pf-gpu-compute__pad', 'Analysis plugin not enabled.');
      }
    } else {
      body = m(KernelMetricsSection, {
        ctx: this.ctx,
        engine: this.engine,
        sliceId: effectiveSliceId,
        baseline: this.baselineData,
        analysisCache: this.analysisCache,
      });
    }

    return m('div', [toolbar, body]);
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
    trace.commands.registerCommand({
      id: `${GpuComputePlugin.id}#ShowComputeTab`,
      name: 'Show Compute Tab',
      callback: () => trace.tabs.showTab(tabUri),
    });

    const content = new Compute(
      trace.engine,
      trace,
      tabUri,
      this.terminologyRegistry,
      this.sectionRegistry,
      this.analysisProviderHolder,
    );
    trace.tabs.registerTab({
      isEphemeral: false,
      uri: tabUri,
      content: content,
    });

    try {
      const rows = await fetchKernelSummaryRows(
        this.getContext(),
        trace.engine,
      );
      content.setSummaryRows(rows);
      content.setOptions(rows.map((r) => ({id: r.id, label: r.demangledName})));
      if (rows.length > 0) {
        trace.tabs.showTab(tabUri);
      }
    } catch (e) {
      console.warn('GpuCompute: failed to fetch kernel launch list:', e);
      content.setOptions([]);
    }
  }
}
