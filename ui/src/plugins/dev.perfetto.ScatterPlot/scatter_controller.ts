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

import {clamp} from '../../base/math_utils';
import type {Engine} from '../../trace_processor/engine';
import type {SqlValue} from '../../trace_processor/query_result';
import {buildOperatorPointSet, nearest} from './point_set';
import {
  ScatterSource,
  type PreparedSource,
  type ViewportRequest,
} from './scatter_source';
import type {
  ColorMode,
  ColumnInfo,
  DataBounds,
  PlotSpec,
  PointColumns,
  PointSet,
  SourceSpec,
  ViewRange,
} from './types';
import {NULL_CATEGORY} from './types';
import {clampView, fitView, viewsEqual} from './view';

export type LoadState =
  | {readonly kind: 'empty'}
  | {readonly kind: 'describing'}
  | {readonly kind: 'needs_axes'}
  | {readonly kind: 'preparing'}
  | {readonly kind: 'loading'; readonly total?: number}
  | {readonly kind: 'ready'}
  | {readonly kind: 'error'; readonly message: string};

export interface PointRef {
  readonly points: PointSet;
  readonly index: number;
}

export interface RowDetails {
  readonly row: number;
  readonly cells: ReadonlyArray<{
    readonly name: string;
    readonly value: SqlValue;
  }>;
}

export interface BrushState {
  readonly rect: DataBounds;
  readonly count: number | undefined;
}

export interface ScatterSourceLike extends AsyncDisposable {
  describe(spec: SourceSpec): Promise<ReadonlyArray<ColumnInfo>>;
  prepare(spec: SourceSpec, plot: PlotSpec): Promise<PreparedSource>;
  dispose(): Promise<void>;
}

export interface ScatterControllerOptions {
  readonly engine: Engine;
  readonly uid: string;
  readonly onChange: () => void;
  readonly viewportDebounceMs?: number;
  readonly sourceFactory?: (engine: Engine, uid: string) => ScatterSourceLike;
}

const DEFAULT_VIEWPORT_DEBOUNCE_MS = 20;
const HOVER_DEBOUNCE_MS = 150;

export interface InternalViewportRequest extends ViewportRequest {
  readonly cellX: number;
  readonly cellY: number;
  readonly lx: number;
  readonly ly: number;
}

export interface AlignedAxis {
  readonly min: number;
  readonly max: number;
  readonly count: number;
  readonly cell: number;
  readonly l: number;
}

export function computeAlignedAxis(
  v0: number,
  v1: number,
  dMin: number,
  dMax: number,
  plotPx: number,
  pointSizePx: number,
): AlignedAxis {
  const span = dMax - dMin;
  if (span <= 0 || !Number.isFinite(span)) {
    return {min: dMin - 0.5, max: dMax + 0.5, count: 1, cell: 1, l: 0};
  }

  const viewSpan = Math.max(1e-9, v1 - v0);
  const desiredCellData =
    (viewSpan / Math.max(1, plotPx)) * Math.max(1, pointSizePx);
  const rawL = Math.round(Math.log2(span / desiredCellData));
  const l = clamp(Number.isFinite(rawL) ? rawL : 0, 0, 20);
  const numCells = Math.pow(2, l);
  const cell = span / numCells;
  const pad = viewSpan * 0.15;

  let kMin = Math.floor((v0 - pad - dMin) / cell);
  let kMax = Math.ceil((v1 + pad - dMin) / cell);
  kMin = clamp(kMin, 0, numCells - 1);
  kMax = clamp(kMax, kMin + 1, numCells);

  return {
    min: dMin + kMin * cell,
    max: dMin + kMax * cell,
    count: kMax - kMin,
    cell,
    l,
  };
}

export function computeViewportRequest(
  view: ViewRange,
  bounds: DataBounds,
  plotW: number,
  plotH: number,
  pointSize: number,
): InternalViewportRequest {
  const ax = computeAlignedAxis(
    view.x0,
    view.x1,
    bounds.xMin,
    bounds.xMax,
    plotW,
    pointSize,
  );
  const ay = computeAlignedAxis(
    view.y0,
    view.y1,
    bounds.yMin,
    bounds.yMax,
    plotH,
    pointSize,
  );
  return {
    xMin: ax.min,
    xMax: ax.max,
    yMin: ay.min,
    yMax: ay.max,
    cols: ax.count,
    rows: ay.count,
    cellX: ax.cell,
    cellY: ay.cell,
    lx: ax.l,
    ly: ay.l,
  };
}

export function shouldRequery(
  view: ViewRange,
  bounds: DataBounds,
  needed: {readonly lx: number; readonly ly: number},
  lastFetched?: {
    readonly xMin: number;
    readonly xMax: number;
    readonly yMin: number;
    readonly yMax: number;
    readonly lx: number;
    readonly ly: number;
  },
): boolean {
  if (lastFetched === undefined) return true;
  if (needed.lx !== lastFetched.lx || needed.ly !== lastFetched.ly) return true;

  const x0 = Math.max(view.x0, bounds.xMin);
  const x1 = Math.min(view.x1, bounds.xMax);
  const y0 = Math.max(view.y0, bounds.yMin);
  const y1 = Math.min(view.y1, bounds.yMax);

  if (x0 <= x1 && (x0 < lastFetched.xMin || x1 > lastFetched.xMax)) return true;
  if (y0 <= y1 && (y0 < lastFetched.yMin || y1 > lastFetched.yMax)) return true;
  return false;
}

export class ScatterController implements AsyncDisposable {
  private readonly viewportDebounceMs: number;
  private readonly sourceInstance: ScatterSourceLike;

  private disposed = false;
  private generation = 0;
  private loadAbortController?: AbortController;

  private loadState: LoadState = {kind: 'empty'};
  private currentSource?: SourceSpec;
  private columnsList: ReadonlyArray<ColumnInfo> = [];
  private currentPlot?: PlotSpec;
  private totalRowCount?: number;
  private currentDataBounds?: DataBounds;
  private currentColorMode: ColorMode = {kind: 'none'};
  private readonly hiddenCats = new Set<number>();
  private currentView?: ViewRange;

  private plotWidthPx = 800;
  private plotHeightPx = 600;
  private pointSizePx = 3;

  private lastFetchedRegion?: InternalViewportRequest;
  // hiddenKey() of the categories excluded from the last fetched viewport.
  private lastFetchedHiddenKey = '';
  private viewportToken = 0;
  private viewportTimer?: ReturnType<typeof setTimeout>;

  private updating = false;
  private queryDurationMs?: number;
  private resultRowCount?: number;

  private baseSet?: PointSet;
  private preparedSource?: PreparedSource;

  private hoverPoint?: PointRef;
  private hoverDetailsData?: RowDetails;
  private hoverTimer?: ReturnType<typeof setTimeout>;
  private hoverToken = 0;

  private pinnedPoint?: PointRef;
  private pinnedDetailsData?: RowDetails;
  private pinToken = 0;

  private brushState?: BrushState;
  private brushToken = 0;

  private updatingTimer?: ReturnType<typeof setTimeout>;
  private lastViewEmitMs = 0;
  private viewEmitTimer?: ReturnType<typeof setTimeout>;

  constructor(private readonly opts: ScatterControllerOptions) {
    this.viewportDebounceMs =
      opts.viewportDebounceMs ?? DEFAULT_VIEWPORT_DEBOUNCE_MS;
    const factory =
      opts.sourceFactory ??
      ((engine: Engine, uid: string) => new ScatterSource(engine, uid));
    this.sourceInstance = factory(opts.engine, opts.uid);
  }

  get state(): LoadState {
    return this.loadState;
  }
  get source(): SourceSpec | undefined {
    return this.currentSource;
  }
  get columns(): ReadonlyArray<ColumnInfo> {
    return this.columnsList;
  }
  get plot(): PlotSpec | undefined {
    return this.currentPlot;
  }
  get rowCount(): number | undefined {
    return this.totalRowCount;
  }
  get dataBounds(): DataBounds | undefined {
    return this.currentDataBounds;
  }
  get colorMode(): ColorMode {
    return this.currentColorMode;
  }
  get hiddenCategories(): ReadonlySet<number> {
    return this.hiddenCats;
  }
  get view(): ViewRange | undefined {
    return this.currentView;
  }
  get hover(): PointRef | undefined {
    return this.hoverPoint;
  }
  get pinned(): PointRef | undefined {
    return this.pinnedPoint;
  }
  get pinnedDetails(): RowDetails | undefined {
    return this.pinnedDetailsData;
  }
  get hoverDetails(): RowDetails | undefined {
    return this.hoverDetailsData;
  }
  get brush(): BrushState | undefined {
    return this.brushState;
  }
  get isUpdating(): boolean {
    return this.updating;
  }
  get lastQueryMs(): number | undefined {
    return this.queryDurationMs;
  }
  get lastResultRows(): number | undefined {
    return this.resultRowCount;
  }
  get points(): PointSet | undefined {
    return this.baseSet;
  }

  setPlotSize(widthPx: number, heightPx: number, pointSizePx: number): void {
    const w = Math.max(1, Math.round(widthPx));
    const h = Math.max(1, Math.round(heightPx));
    const pt = Math.max(1, pointSizePx);
    if (
      this.plotWidthPx === w &&
      this.plotHeightPx === h &&
      this.pointSizePx === pt
    ) {
      return;
    }
    this.plotWidthPx = w;
    this.plotHeightPx = h;
    this.pointSizePx = pt;
    if (this.currentView !== undefined && this.preparedSource !== undefined) {
      this.requestViewport();
    }
  }

  async setSource(spec: SourceSpec): Promise<void> {
    if (this.disposed) return;
    const gen = ++this.generation;
    this.abortOngoingWork();
    this.clearInteractions();
    this.currentSource = spec;
    this.loadState = {kind: 'describing'};
    this.emitChange();

    const loadAbort = new AbortController();
    this.loadAbortController = loadAbort;

    try {
      const cols = await this.sourceInstance.describe(spec);
      if (
        this.disposed ||
        gen !== this.generation ||
        loadAbort.signal.aborted
      ) {
        return;
      }
      this.columnsList = cols;
      const plot = this.resolvePlotSpec(cols);
      if (plot === undefined) {
        // Drop the previous source's points/view so stale data isn't shown.
        this.viewportToken++;
        this.preparedSource = undefined;
        this.baseSet = undefined;
        this.currentView = undefined;
        this.currentDataBounds = undefined;
        this.totalRowCount = undefined;
        this.resultRowCount = undefined;
        this.queryDurationMs = undefined;
        this.currentColorMode = {kind: 'none'};
        this.loadState = {kind: 'needs_axes'};
        this.currentPlot = undefined;
        this.emitChange();
        return;
      }
      this.currentPlot = plot;
      await this.executeLoad(gen, spec, plot, true, loadAbort.signal);
    } catch (err: unknown) {
      if (
        this.disposed ||
        gen !== this.generation ||
        loadAbort.signal.aborted
      ) {
        return;
      }
      this.loadState = {
        kind: 'error',
        message: err instanceof Error ? err.message : String(err),
      };
      this.emitChange();
    }
  }

  async setPlot(plot: PlotSpec): Promise<void> {
    if (this.disposed) return;
    const gen = ++this.generation;
    this.abortOngoingWork();
    this.clearInteractions();

    const axesChanged =
      this.currentPlot === undefined ||
      this.currentPlot.x !== plot.x ||
      this.currentPlot.y !== plot.y;
    this.currentPlot = plot;
    if (this.currentSource === undefined) return;

    const loadAbort = new AbortController();
    this.loadAbortController = loadAbort;

    try {
      await this.executeLoad(
        gen,
        this.currentSource,
        plot,
        axesChanged,
        loadAbort.signal,
      );
    } catch (err: unknown) {
      if (
        this.disposed ||
        gen !== this.generation ||
        loadAbort.signal.aborted
      ) {
        return;
      }
      this.loadState = {
        kind: 'error',
        message: err instanceof Error ? err.message : String(err),
      };
      this.emitChange();
    }
  }

  setView(view: ViewRange): void {
    if (this.currentDataBounds === undefined) return;
    const clamped = clampView(view, this.currentDataBounds);
    if (
      this.currentView !== undefined &&
      viewsEqual(this.currentView, clamped)
    ) {
      return;
    }
    this.currentView = clamped;
    this.emitChangeThrottled();
    this.requestViewport();
  }

  resetView(): void {
    if (this.currentDataBounds !== undefined) {
      this.setView(fitView(this.currentDataBounds));
    }
  }

  toggleCategory(index: number): void {
    if (this.hiddenCats.has(index)) {
      this.hiddenCats.delete(index);
    } else {
      this.hiddenCats.add(index);
    }
    this.onCategoriesChanged();
  }

  /**
   * Shows only `index` out of `all` legend indices. If every other entry is
   * already hidden (i.e. `index` is already isolated) this restores all, so
   * repeating the gesture toggles back.
   */
  isolateCategory(index: number, all: ReadonlyArray<number>): void {
    const alreadyIsolated = all.every(
      (i) => i === index || this.hiddenCats.has(i),
    );
    this.hiddenCats.clear();
    if (!alreadyIsolated) {
      for (const i of all) {
        if (i !== index) this.hiddenCats.add(i);
      }
    }
    this.onCategoriesChanged();
  }

  showAllCategories(): void {
    if (this.hiddenCats.size === 0) return;
    this.hiddenCats.clear();
    this.onCategoriesChanged();
  }

  private onCategoriesChanged(): void {
    // Hidden points can't be hovered/pinned, so drop any that just vanished.
    if (this.hoverPoint !== undefined && this.isHidden(this.hoverPoint)) {
      this.setHover(undefined);
    }
    if (this.pinnedPoint !== undefined && this.isHidden(this.pinnedPoint)) {
      this.pin(undefined);
    }
    this.emitChange();
    // The operator returns one representative per cell, so client-side hiding
    // alone would also drop visible points that share a cell with a hidden
    // representative. Re-query with the hidden set excluded server-side; the
    // renderer hides affected points immediately in the meantime.
    this.refetchViewport();
  }

  private hiddenKey(): string {
    return [...this.hiddenCats].sort((a, b) => a - b).join(',');
  }

  private refetchViewport(): void {
    if (
      this.disposed ||
      this.currentView === undefined ||
      this.preparedSource === undefined ||
      this.currentDataBounds === undefined ||
      this.hiddenKey() === this.lastFetchedHiddenKey
    ) {
      return;
    }
    this.clearViewportTimer();
    void this.executeQuery(
      computeViewportRequest(
        this.currentView,
        this.currentDataBounds,
        this.plotWidthPx,
        this.plotHeightPx,
        this.pointSizePx,
      ),
    );
  }

  private isHiddenIndex(points: PointSet, i: number): boolean {
    if (this.hiddenCats.size === 0) return false;
    const mode = this.currentColorMode;
    if (mode.kind === 'categorical') {
      return this.hiddenCats.has(Math.round(points.color[i]));
    }
    // Only NULL can be hidden for uniform/numeric colouring.
    return (
      this.hiddenCats.has(NULL_CATEGORY) &&
      mode.kind === 'numeric' &&
      Number.isNaN(points.color[i])
    );
  }

  private isHidden(ref: PointRef): boolean {
    return this.isHiddenIndex(ref.points, ref.index);
  }

  setHover(ref: PointRef | undefined): void {
    if (
      this.hoverPoint?.points === ref?.points &&
      this.hoverPoint?.index === ref?.index
    ) {
      return;
    }
    this.hoverPoint = ref;
    this.hoverDetailsData = undefined;
    this.clearHoverTimer();
    const token = ++this.hoverToken;
    this.emitChange();

    const prepared = this.preparedSource;
    if (ref === undefined || prepared === undefined) return;

    const rowId = ref.points.row[ref.index];
    this.hoverTimer = setTimeout(async () => {
      this.hoverTimer = undefined;
      if (this.disposed || token !== this.hoverToken) return;
      try {
        const cells = await prepared.fetchRow(rowId);
        if (this.disposed || token !== this.hoverToken) return;
        this.hoverDetailsData = {row: rowId, cells};
        this.emitChange();
      } catch {
        // Ignored.
      }
    }, HOVER_DEBOUNCE_MS);
  }

  pin(ref: PointRef | undefined): void {
    if (
      this.pinnedPoint?.points === ref?.points &&
      this.pinnedPoint?.index === ref?.index
    ) {
      return;
    }
    this.pinnedPoint = ref;
    this.pinnedDetailsData = undefined;
    const token = ++this.pinToken;
    this.emitChange();

    const prepared = this.preparedSource;
    if (ref === undefined || prepared === undefined) return;

    const rowId = ref.points.row[ref.index];
    void (async () => {
      try {
        const cells = await prepared.fetchRow(rowId);
        if (this.disposed || token !== this.pinToken) return;
        this.pinnedDetailsData = {row: rowId, cells};
        this.emitChange();
      } catch {
        // Ignored.
      }
    })();
  }

  setBrush(rect: DataBounds | undefined): void {
    if (rect === undefined) {
      if (this.brushState === undefined) return;
      this.brushState = undefined;
      this.brushToken++;
      this.emitChange();
      return;
    }

    this.brushState = {rect, count: undefined};
    const token = ++this.brushToken;
    this.emitChange();

    const prepared = this.preparedSource;
    if (prepared === undefined) return;

    void (async () => {
      try {
        const count = await prepared.countInRect(rect);
        if (this.disposed || token !== this.brushToken) return;
        this.brushState = {rect, count};
        this.emitChange();
      } catch {
        // Ignored.
      }
    })();
  }

  sqlForBrush(): string | undefined {
    if (this.brushState === undefined || this.preparedSource === undefined) {
      return undefined;
    }
    return this.preparedSource.sqlForRect(this.brushState.rect);
  }

  pick(
    x: number,
    y: number,
    scaleX: number,
    scaleY: number,
    radiusPx: number,
  ): PointRef | undefined {
    if (this.baseSet === undefined) return undefined;
    const set = this.baseSet;
    const skip =
      this.hiddenCats.size > 0
        ? (i: number) => this.isHiddenIndex(set, i)
        : undefined;
    const hit = nearest(set, x, y, scaleX, scaleY, radiusPx, skip);
    return hit !== undefined ? {points: set, index: hit.index} : undefined;
  }

  async dispose(): Promise<void> {
    await this[Symbol.asyncDispose]();
  }

  async [Symbol.asyncDispose](): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    this.abortOngoingWork();
    await this.sourceInstance.dispose();
  }

  private resolvePlotSpec(
    cols: ReadonlyArray<ColumnInfo>,
  ): PlotSpec | undefined {
    if (this.currentPlot !== undefined) {
      const {x, y, color} = this.currentPlot;
      const hasX = cols.some((c) => c.name === x && c.kind === 'numeric');
      const hasY = cols.some((c) => c.name === y && c.kind === 'numeric');
      const hasColor =
        color === undefined || cols.some((c) => c.name === color);
      if (hasX && hasY && hasColor) return this.currentPlot;
    }

    const numericCols = cols.filter((c) => c.kind === 'numeric');
    const hasTs = numericCols.some((c) => c.name === 'ts');
    const hasDur = numericCols.some((c) => c.name === 'dur');
    if (hasTs && hasDur) return {x: 'ts', y: 'dur'};
    if (numericCols.length >= 2) {
      return {x: numericCols[0].name, y: numericCols[1].name};
    }
    return undefined;
  }

  private async executeLoad(
    gen: number,
    spec: SourceSpec,
    plot: PlotSpec,
    resetViewToFit: boolean,
    signal: AbortSignal,
  ): Promise<void> {
    this.preparedSource = undefined;
    this.baseSet = undefined;
    this.viewportToken++;
    this.hiddenCats.clear();
    this.loadState = {kind: 'preparing'};
    this.emitChange();

    this.lastFetchedRegion = undefined;
    this.lastFetchedHiddenKey = '';
    this.queryDurationMs = undefined;
    this.resultRowCount = undefined;

    const prepared = await this.sourceInstance.prepare(spec, plot);
    if (this.disposed || gen !== this.generation || signal.aborted) return;

    this.preparedSource = prepared;
    this.totalRowCount = prepared.count;
    this.currentDataBounds = prepared.bounds;
    this.currentColorMode = prepared.colorMode;

    if (resetViewToFit || this.currentView === undefined) {
      this.currentView = fitView(prepared.bounds);
    }

    if (prepared.count === 0) {
      this.baseSet = buildOperatorPointSet(
        {
          x: new Float64Array(0),
          y: new Float64Array(0),
          row: new Float64Array(0),
          color: new Float32Array(0),
        },
        prepared.bounds,
      );
      this.loadState = {kind: 'ready'};
      this.resultRowCount = 0;
      this.emitChange();
      return;
    }

    this.loadState = {kind: 'loading', total: prepared.count};
    this.emitChange();

    await this.executeQuery(
      computeViewportRequest(
        this.currentView,
        prepared.bounds,
        this.plotWidthPx,
        this.plotHeightPx,
        this.pointSizePx,
      ),
    );
  }

  private requestViewport(): void {
    if (
      this.disposed ||
      this.currentView === undefined ||
      this.preparedSource === undefined ||
      this.currentDataBounds === undefined
    ) {
      return;
    }

    const req = computeViewportRequest(
      this.currentView,
      this.currentDataBounds,
      this.plotWidthPx,
      this.plotHeightPx,
      this.pointSizePx,
    );
    if (
      !shouldRequery(
        this.currentView,
        this.currentDataBounds,
        req,
        this.lastFetchedRegion,
      ) &&
      this.hiddenKey() === this.lastFetchedHiddenKey
    ) {
      return;
    }

    this.clearViewportTimer();
    if (this.viewportDebounceMs === 0 || this.lastFetchedRegion === undefined) {
      void this.executeQuery(req);
    } else {
      this.viewportTimer = setTimeout(() => {
        this.viewportTimer = undefined;
        void this.executeQuery(req);
      }, this.viewportDebounceMs);
    }
  }

  flushViewport(): void {
    if (this.viewportTimer === undefined) return;
    this.clearViewportTimer();
    if (
      this.disposed ||
      this.currentView === undefined ||
      this.preparedSource === undefined ||
      this.currentDataBounds === undefined
    ) {
      return;
    }
    void this.executeQuery(
      computeViewportRequest(
        this.currentView,
        this.currentDataBounds,
        this.plotWidthPx,
        this.plotHeightPx,
        this.pointSizePx,
      ),
    );
  }

  private async executeQuery(req: InternalViewportRequest): Promise<void> {
    if (this.disposed || this.preparedSource === undefined) return;

    const token = ++this.viewportToken;
    this.updating = true;
    this.clearUpdatingTimer();
    this.updatingTimer = setTimeout(() => {
      this.updatingTimer = undefined;
      if (this.updating && !this.disposed && token === this.viewportToken) {
        this.emitChange();
      }
    }, 150);

    const prepared = this.preparedSource;
    const startMs = performance.now();
    const hiddenKey = this.hiddenKey();
    const hidden = [...this.hiddenCats].sort((a, b) => a - b);

    try {
      const res = await prepared.fetchViewport({...req, hidden});
      if (this.disposed || token !== this.viewportToken) return;

      this.queryDurationMs = Math.round(performance.now() - startMs);
      this.resultRowCount = res.x.length;
      this.lastFetchedRegion = req;
      this.lastFetchedHiddenKey = hiddenKey;

      const n = res.count.length;
      const weight = new Float32Array(n);
      for (let i = 0; i < n; i++) {
        weight[i] = Math.min(16_777_216, res.count[i]);
      }
      const colsWithWeight: PointColumns = {
        x: res.x,
        y: res.y,
        row: res.row,
        color: res.color,
        weight,
      };
      const newSet = buildOperatorPointSet(colsWithWeight, {
        xMin: req.xMin,
        xMax: req.xMax,
        yMin: req.yMin,
        yMax: req.yMax,
      });

      this.clearUpdatingTimer();
      this.baseSet = newSet;
      this.updating = false;
      if (
        this.loadState.kind === 'loading' ||
        this.loadState.kind === 'preparing'
      ) {
        this.loadState = {kind: 'ready'};
      }
      this.emitChange();
    } catch (err: unknown) {
      if (this.disposed || token !== this.viewportToken) return;
      this.clearUpdatingTimer();
      this.updating = false;
      this.loadState = {
        kind: 'error',
        message: err instanceof Error ? err.message : String(err),
      };
      this.emitChange();
    }
  }

  private abortOngoingWork(): void {
    this.viewportToken++;
    this.loadAbortController?.abort();
    this.loadAbortController = undefined;
    this.clearViewportTimer();
    this.clearHoverTimer();
    this.clearUpdatingTimer();
    this.clearViewEmitTimer();
  }

  private clearInteractions(): void {
    this.hoverPoint = undefined;
    this.hoverDetailsData = undefined;
    this.pinnedPoint = undefined;
    this.pinnedDetailsData = undefined;
    this.brushState = undefined;
    this.hoverToken++;
    this.pinToken++;
    this.brushToken++;
  }

  private clearViewportTimer(): void {
    if (this.viewportTimer !== undefined) {
      clearTimeout(this.viewportTimer);
      this.viewportTimer = undefined;
    }
  }
  private clearHoverTimer(): void {
    if (this.hoverTimer !== undefined) {
      clearTimeout(this.hoverTimer);
      this.hoverTimer = undefined;
    }
  }
  private clearUpdatingTimer(): void {
    if (this.updatingTimer !== undefined) {
      clearTimeout(this.updatingTimer);
      this.updatingTimer = undefined;
    }
  }
  private clearViewEmitTimer(): void {
    if (this.viewEmitTimer !== undefined) {
      clearTimeout(this.viewEmitTimer);
      this.viewEmitTimer = undefined;
    }
  }

  private emitChangeThrottled(): void {
    if (this.disposed) return;
    const now = performance.now();
    const elapsed = now - this.lastViewEmitMs;
    if (elapsed >= 250) {
      this.lastViewEmitMs = now;
      this.clearViewEmitTimer();
      this.emitChange();
    } else if (this.viewEmitTimer === undefined) {
      this.viewEmitTimer = setTimeout(() => {
        this.viewEmitTimer = undefined;
        this.lastViewEmitMs = performance.now();
        this.emitChange();
      }, 250 - elapsed);
    }
  }

  private emitChange(): void {
    if (!this.disposed) this.opts.onChange();
  }
}
