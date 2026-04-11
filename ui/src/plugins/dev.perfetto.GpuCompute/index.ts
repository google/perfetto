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
import {
  KernelMetricsSection,
  fetchKernelLaunchList,
  fetchSelectedKernelMetricData,
} from './details';
import {TrackEventSelection} from '../../public/selection';
import {
  getActiveInfoTab,
  renderToolbar,
  setInfoTab,
  resetToolbarState,
} from './toolbar';
import type {
  KernelLaunchOption,
  KernelMetricData,
  ToolbarInfo,
} from './details';
import {enableHumanizeMetrics} from './toolbar';
import {KernelSummarySection} from './summary';
import {registerSpeedOfLightSection} from './section/speed_of_light';
import {registerLaunchStatisticsSection} from './section/launch_statistics';
import {registerOccupancySection} from './section/occupancy';
import {registerWorkloadAnalysisSection} from './section/workload_analysis';
import {getTerminologyId} from './terminology';
import {registerCUDATerminology} from './terminology/cuda';
import {registerOpenCLTerminology} from './terminology/opencl';
import {
  PerformanceAnalysisResult,
  AnalysisCache,
  getAnalysisProvider,
} from './analysis';

class Compute {
  constructor(
    private engine: Engine,
    _trace: Trace,
  ) {}

  private sliceId: number | undefined = -1;
  private options: KernelLaunchOption[] = [];

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
    return 'Compute';
  }

  // Updates which kernel is shown. If sliceId is undefined, it falls back to -1 (invalid selection).
  // Also updates which tab is shown based on the current selection.
  // When `prefetchedToolbar` is provided, the internal fetch is skipped.
  public async setSliceId(
    sliceId?: number,
    suppressAutoDetails = false,
    prefetchedToolbar?: {hasMetrics: boolean; toolbar?: ToolbarInfo},
  ) {
    const firstId = this.options[0]?.id ?? -1;
    const valid = sliceId != null && sliceId !== -1;
    const targetId = valid ? sliceId! : firstId;

    const fetchToolbar = async (id: number) => {
      try {
        const data = await fetchSelectedKernelMetricData(this.engine, id);
        const hasMetrics = Array.isArray(data) && data.length > 0;
        const toolbar = hasMetrics ? data[0].toolbar : undefined;
        return {hasMetrics, toolbar};
      } catch (e) {
        console.warn('GpuCompute: failed to fetch toolbar metrics:', e);
        return {hasMetrics: false, toolbar: undefined};
      }
    };

    // Auto displays summary when no valid slice selection is made
    const useSummary = async (id: number) => {
      this.sliceId = id;
      if (id !== -1) {
        const toolbarResponse = await fetchToolbar(id);
        this.setToolbarInfo(toolbarResponse.toolbar);
      } else {
        this.setToolbarInfo(undefined);
      }
      setInfoTab('summary');
      m.redraw();
    };

    // Invalid selection
    if (!valid) {
      await useSummary(targetId);
      return;
    }

    // Valid selection
    this.sliceId = targetId;
    m.redraw();

    const toolbarResponse =
      prefetchedToolbar ??
      (targetId !== -1
        ? await fetchToolbar(targetId)
        : {hasMetrics: false, toolbar: undefined});

    // Selected slice has no metrics, so we fall back to the first launch in the toolbar and show the summary tab.
    if (!toolbarResponse.hasMetrics) {
      await useSummary(firstId);
      return;
    }

    // Valid and has metrics so we show the details tab (unless suppressed)
    this.setToolbarInfo(toolbarResponse.toolbar);
    if (!suppressAutoDetails) {
      setInfoTab('details');
    }
  }

  private async setBaselineId(useCurrent: boolean) {
    // If not using current, clear baseline state
    if (!useCurrent) {
      this.baselineSliceId = undefined;
      this.baselineToolbarInfo = undefined;
      this.baselineData = undefined;
      m.redraw();
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
      const data = await fetchSelectedKernelMetricData(this.engine, id);
      this.baselineToolbarInfo = data?.[0]?.toolbar;
      this.baselineData = data?.[0];
    } catch {
      this.baselineToolbarInfo = undefined;
      this.baselineData = undefined;
    }

    m.redraw();
  }

  // Instead of resetting the baseline use refresh to avoid switching the baselineId
  private async refreshBaseline() {
    if (this.baselineSliceId == null) return;

    try {
      const data = await fetchSelectedKernelMetricData(
        this.engine,
        this.baselineSliceId,
      );
      this.baselineToolbarInfo = data?.[0]?.toolbar;
      this.baselineData = data?.[0];
    } catch {
      this.baselineToolbarInfo = undefined;
      this.baselineData = undefined;
    }

    m.redraw();
  }

  // Updates the "Results" dropdown with new launch options and triggers a redraw
  public setOptions(opts: KernelLaunchOption[]) {
    this.options = opts;
    m.redraw();
  }

  // Populating the toolbar with the correct kernel's launch info
  private toolbarInfo?: ToolbarInfo;
  public setToolbarInfo(info?: ToolbarInfo) {
    this.toolbarInfo = info;
    m.redraw();
  }

  // Used to re-fetch the same items so both toolbar + tables reflect new mode
  private async refresh() {
    if (this.sliceId != null && this.sliceId !== -1) {
      await this.setSliceId(this.sliceId);
    }

    // If there’s an active baseline, re-fetch it for the same sliceId (don’t overwrite it with current!)
    if (this.baselineSliceId != null) {
      await this.refreshBaseline();
    }
  }

  // Fetch and process all kernel-related performance metric data from the trace
  // If a slice is selected, we pass sliceId so the component shows only the selected kernel compute metrics if they are available
  render(): m.Children {
    // The key forces a remount when the inputs change so the data refreshes
    const wrapKey = `view:selected:${this.sliceId ?? 'none'}:baseline:${this.baselineSliceId ?? 'none'}:term:${getTerminologyId()}:humanize:${enableHumanizeMetrics ? 1 : 0}`;

    const toolbar = renderToolbar({
      options: this.options,
      sliceId: this.sliceId ?? undefined,
      onChange: (id, suppress) => this.setSliceId(id, suppress),
      toolbarInfo: this.toolbarInfo,
      baselineId: this.baselineSliceId,
      baselineInfo: this.baselineToolbarInfo,
      baselineEnabled: this.baselineSliceId != null,
      onHumanizeChanged: () => this.refresh(),
      onToggleBaseline: (enabled: boolean) => this.setBaselineId(enabled),
      onTerminologyChanged: () => this.refresh(),
    });

    // if sliceId !== -1 -> we show that specific kernel's compute metrics or fallback to a no metrics message if none are available
    const effectiveSliceId = this.sliceId ?? -1;
    let body: m.Children;
    const activeTab = getActiveInfoTab();

    if (activeTab === 'summary') {
      body = m(KernelSummarySection, {
        engine: this.engine,
        sliceId: effectiveSliceId,
        openSliceInDetail: (id: number) => this.setSliceId(id),
      });
    } else if (activeTab === 'analysis') {
      const provider = getAnalysisProvider();
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
        engine: this.engine,
        sliceId: effectiveSliceId,
        baseline: this.baselineData,
        analysisCache: this.analysisCache,
      });
    }

    // Rendering view
    return m('div', {key: wrapKey}, [toolbar, body]);
  }
}

// Plugin entry point, registers the tab and menu command
export default class GpuComputePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GpuCompute';

  static onActivate(): void {
    registerCUDATerminology();
    registerOpenCLTerminology();
    registerSpeedOfLightSection();
    registerLaunchStatisticsSection();
    registerOccupancySection();
    registerWorkloadAnalysisSection();
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    resetToolbarState();

    const tabUri = `${GpuComputePlugin.id}#Compute`;
    trace.commands.registerCommand({
      id: `${GpuComputePlugin.id}#ShowComputeTab`,
      name: 'Show Compute Tab',
      callback: () => trace.tabs.showTab(tabUri),
    });

    const content = new Compute(trace.engine, trace);
    trace.tabs.registerTab({
      isEphemeral: false,
      uri: tabUri,
      content: content,
    });

    // Load dropdown options once
    await fetchKernelLaunchList(trace.engine)
      .then((list) => content.setOptions(list))
      .catch((e) => {
        console.warn('GpuCompute: failed to fetch kernel launch list:', e);
        content.setOptions([]);
      });

    // Observe selection changes via a periodic check.
    // When a compute slice is selected, fetch its metrics, update state,
    // and auto-show the tab. The interval only reads a property on each
    // tick; SQL queries are only issued when the selection actually changes.
    // TODO: Replace this polling with a proper selection change observer API
    // once one is available (e.g. trace.selection.onChange).
    let lastSliceId: number | undefined;
    let selectionGeneration = 0;
    const selectionInterval = setInterval(() => {
      const sel = trace.selection.selection;
      const sliceId =
        sel.kind === 'track_event'
          ? (sel as TrackEventSelection).eventId
          : undefined;
      if (sliceId === lastSliceId) return;
      lastSliceId = sliceId;
      const gen = ++selectionGeneration;

      if (sliceId !== undefined) {
        fetchSelectedKernelMetricData(trace.engine, sliceId)
          .then((data) => {
            if (gen !== selectionGeneration) return;
            const hasMetrics = Array.isArray(data) && data.length > 0;
            const toolbar = hasMetrics ? data[0].toolbar : undefined;
            content.setSliceId(sliceId, false, {hasMetrics, toolbar});
            if (hasMetrics) {
              trace.tabs.showTab(tabUri);
            }
          })
          .catch((e) => {
            if (gen !== selectionGeneration) return;
            console.warn('GpuCompute: failed to fetch slice metrics:', e);
            content.setSliceId(sliceId, false, {
              hasMetrics: false,
              toolbar: undefined,
            });
          });
      } else {
        content.setSliceId(undefined);
      }
    }, 100);

    trace.trash.use({
      [Symbol.dispose]: () => {
        clearInterval(selectionInterval);
      },
    });
  }
}
