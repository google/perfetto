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
import {Trace} from '../../../../public/trace';
import {VisualisationNode, ChartConfig} from '../nodes/visualisation_node';
import {SQLBarChartLoader} from '../../../../components/widgets/charts/bar_chart_loader';
import {SQLHistogramLoader} from '../../../../components/widgets/charts/histogram_loader';
import {
  ChartLoaderEntry,
  ChartRenderContext,
  renderBarChart,
  renderHistogram,
} from './chart_renderers';
import {renderChartConfigPopup} from './chart_config_popup';
import {Button} from '../../../../widgets/button';
import {classNames} from '../../../../base/classnames';
import {Popup, PopupPosition} from '../../../../widgets/popup';
import {Select} from '../../../../widgets/select';
import {ResizeHandle} from '../../../../widgets/resize_handle';
import {QueryExecutionService} from '../query_execution_service';
import {EmptyState} from '../../../../widgets/empty_state';
import {Card} from '../../../../widgets/card';
import {AddItemPlaceholder} from '../widgets';

export interface ChartViewAttrs {
  trace: Trace;
  node: VisualisationNode;
  queryExecutionService: QueryExecutionService;
  onFilterChange?: () => void;
}

interface ChartViewState {
  loaders: Map<string, ChartLoaderEntry>;
  containerWidth: number;
  editingChartId?: string;
  draggingChartId?: string;
  dragOverChartId?: string;
  tableName?: string;
}

export class ChartView implements m.ClassComponent<ChartViewAttrs> {
  private state: ChartViewState = {
    loaders: new Map(),
    containerWidth: 600,
  };

  private resizeObserver?: ResizeObserver;
  private binCountDebounceTimeout?: ReturnType<typeof setTimeout>;
  private isResolvingTableName = false;

  oncreate({dom, attrs}: m.VnodeDOM<ChartViewAttrs>) {
    const container = dom as HTMLElement;
    this.resizeObserver = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const {width} = entry.contentRect;
        if (width !== this.state.containerWidth) {
          this.state.containerWidth = width;
          m.redraw();
        }
      }
    });
    this.resizeObserver.observe(container);
    this.resolveTableName(attrs);
  }

  onremove() {
    this.resizeObserver?.disconnect();
    if (this.binCountDebounceTimeout !== undefined) {
      clearTimeout(this.binCountDebounceTimeout);
    }
    for (const entry of this.state.loaders.values()) {
      entry.barLoader?.dispose();
      entry.histogramLoader?.dispose();
    }
    this.state.loaders.clear();
  }

  onupdate({attrs}: m.VnodeDOM<ChartViewAttrs>) {
    this.resolveTableName(attrs);
  }

  /**
   * Resolve the materialized table name from the query execution service.
   * When the table name changes, loaders are recreated on the next view().
   *
   * The `isResolvingTableName` flag prevents concurrent resolutions. If an
   * `onupdate` fires while a resolution is in flight the call is a no-op, but
   * the completion of the current resolution always calls `m.redraw()` when the
   * table name changes, which triggers another `onupdate` → another resolution.
   * The state is therefore eventually consistent within one render cycle.
   */
  private async resolveTableName(attrs: ChartViewAttrs): Promise<void> {
    if (this.isResolvingTableName) return;
    this.isResolvingTableName = true;
    try {
      const tableName = await attrs.queryExecutionService.getTableName(
        attrs.node.nodeId,
      );
      if (tableName !== undefined && tableName !== this.state.tableName) {
        this.state.tableName = tableName;
        m.redraw();
      }
    } finally {
      this.isResolvingTableName = false;
    }
  }

  /**
   * Get or create a loader for the given chart config.
   * Disposes and recreates the loader when the config changes.
   */
  private ensureLoader(
    attrs: ChartViewAttrs,
    config: ChartConfig,
  ): ChartLoaderEntry {
    const tableName = this.state.tableName;

    // No table or no column — return empty entry.
    const needsColumn = config.column === '';
    if (tableName === undefined || needsColumn) {
      let entry = this.state.loaders.get(config.id);
      if (!entry) {
        entry = {key: ''};
        this.state.loaders.set(config.id, entry);
      }
      return entry;
    }

    // Key encodes fields that affect loader behaviour for the currently
    // implemented chart types (bar, histogram).  When line/scatter/treemap are
    // added, extend this to include yColumn, groupColumn and sizeColumn.
    const key = [
      tableName,
      config.chartType,
      config.column,
      config.measureColumn ?? '',
    ].join('|');
    const existing = this.state.loaders.get(config.id);

    if (existing && existing.key === key) return existing;

    // Dispose old loaders before creating new ones.
    if (existing) {
      existing.barLoader?.dispose();
      existing.histogramLoader?.dispose();
    }

    const entry: ChartLoaderEntry = {key};
    this.state.loaders.set(config.id, entry);

    const engine = attrs.trace.engine;

    if (config.chartType === 'bar') {
      entry.barLoader = new SQLBarChartLoader({
        engine,
        query: `SELECT * FROM ${tableName}`,
        dimensionColumn: config.column,
        measureColumn: config.measureColumn ?? config.column,
      });
    } else if (config.chartType === 'histogram') {
      // Histograms only need the value column — avoid scanning every column.
      entry.histogramLoader = new SQLHistogramLoader({
        engine,
        query: `SELECT ${config.column} FROM ${tableName}`,
        valueColumn: config.column,
      });
    }

    return entry;
  }

  view({attrs}: m.CVnode<ChartViewAttrs>) {
    const configs = attrs.node.state.chartConfigs;
    const filters = attrs.node.state.chartFilters ?? [];
    const hasFilters = filters.some((f) => f.enabled !== false);

    if (configs.length === 0) {
      return m(
        '.pf-chart-view',
        m(
          EmptyState,
          {
            icon: 'bar_chart',
            title: 'No charts configured',
            fillHeight: true,
          },
          m(Button, {
            label: 'Add first chart',
            icon: 'add',
            onclick: () => {
              attrs.node.addChart();
              attrs.onFilterChange?.();
            },
          }),
        ),
      );
    }

    const toolbar = m('.pf-chart-view__toolbar', [
      m(
        '.pf-chart-view__title',
        configs.length === 1 ? 'Chart' : `${configs.length} Charts`,
      ),
      hasFilters &&
        m(Button, {
          label: 'Clear All Filters',
          icon: 'filter_list_off',
          compact: true,
          onclick: () => {
            attrs.node.clearChartFilters();
            attrs.onFilterChange?.();
          },
        }),
    ]);

    const gridClass = this.getGridClass(configs.length);

    return m('.pf-chart-view', [
      toolbar,
      m(`.pf-chart-view__charts.${gridClass}`, [
        ...configs.map((config) => this.renderSingleChart(attrs, config)),
        m(AddItemPlaceholder, {
          key: 'add-chart',
          label: 'Add Chart',
          icon: 'add',
          onclick: () => {
            attrs.node.addChart();
            attrs.onFilterChange?.();
          },
        }),
      ]),
    ]);
  }

  private getGridClass(chartCount: number): string {
    if (chartCount === 1) return 'pf-chart-grid--1';
    if (chartCount === 2) return 'pf-chart-grid--2';
    if (chartCount <= 4) return 'pf-chart-grid--2x2'; // 3 or 4 → 2×2 grid
    return 'pf-chart-grid--3'; // 5+ → 3-column grid
  }

  private getDefaultChartLabel(config: ChartConfig): string {
    if (config.chartType === 'histogram') return `Histogram: ${config.column}`;
    // bar
    const agg = config.aggregation ?? 'COUNT';
    if (agg === 'COUNT') return `Count by ${config.column}`;
    return `${agg}(${config.measureColumn ?? config.column}) by ${config.column}`;
  }

  private getDefaultChartWidth(chartCount: number): number {
    // 40px = left + right padding of the chart view container
    const containerWidth = this.state.containerWidth - 40;
    if (chartCount === 1) {
      // 32px = card padding (left + right)
      return Math.max(300, containerWidth - 32);
    }
    // 20px = gap between two charts; 32px = card padding per chart
    return Math.max(250, (containerWidth - 20) / 2 - 32);
  }

  private renderSingleChart(
    attrs: ChartViewAttrs,
    config: ChartConfig,
  ): m.Child {
    const ctx: ChartRenderContext = {
      node: attrs.node,
      onFilterChange: attrs.onFilterChange,
    };

    const entry = this.ensureLoader(attrs, config);

    const needsColumn = config.column === '';

    let chartContent: m.Child;

    if (needsColumn) {
      const chartableColumns = attrs.node.getChartableColumns(config.chartType);
      chartContent = m('.pf-chart-view__column-picker', [
        m(EmptyState, {icon: 'ssid_chart', title: 'Select a column'}),
        m(
          Select,
          {
            value: '',
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              attrs.node.updateChart(config.id, {column: target.value});
            },
          },
          [
            m('option', {value: '', disabled: true}, 'Select column...'),
            ...chartableColumns.map((col) =>
              m('option', {value: col.name}, col.name),
            ),
          ],
        ),
      ]);
    } else if (config.chartType === 'bar') {
      chartContent = renderBarChart(ctx, config, entry);
    } else {
      chartContent = renderHistogram(ctx, config, entry);
    }

    const isEditing = this.state.editingChartId === config.id;
    const headerLabel = config.name ?? this.getDefaultChartLabel(config);

    const headerTextContent = isEditing
      ? m('input.pf-chart-view__single-header-input', {
          type: 'text',
          value: config.name ?? '',
          placeholder: this.getDefaultChartLabel(config),
          oncreate: (vnode: m.VnodeDOM) => {
            const input = vnode.dom as HTMLInputElement;
            input.focus();
            input.select();
          },
          onblur: (e: Event) => {
            const target = e.target as HTMLInputElement;
            const name = target.value.trim() || undefined;
            attrs.node.updateChart(config.id, {name});
            this.state.editingChartId = undefined;
          },
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Enter' || e.key === 'Escape') {
              this.state.editingChartId = undefined;
              (e.target as HTMLInputElement).blur();
            }
          },
          oninput: (e: Event) => {
            const target = e.target as HTMLInputElement;
            // Don't trim while typing — let the user type spaces.
            const name = target.value || undefined;
            attrs.node.updateChart(config.id, {name});
          },
        })
      : m(
          '.pf-chart-view__single-header-text',
          {
            onclick: () => {
              this.state.editingChartId = config.id;
            },
            title: 'Click to rename',
          },
          headerLabel,
        );

    const editButton = m(
      Popup,
      {
        trigger: m(Button, {
          icon: 'settings',
          title: 'Edit chart settings',
          compact: true,
        }),
        position: PopupPosition.BottomEnd,
        className: 'pf-chart-config-popup',
      },
      renderChartConfigPopup(ctx, config, () => {
        if (this.binCountDebounceTimeout !== undefined) {
          clearTimeout(this.binCountDebounceTimeout);
        }
        this.binCountDebounceTimeout = setTimeout(() => m.redraw(), 400);
      }),
    );

    const duplicateButton = m(Button, {
      icon: 'content_copy',
      title: 'Duplicate chart',
      compact: true,
      onclick: (e: MouseEvent) => {
        e.stopPropagation();
        attrs.node.duplicateChart(config.id);
        attrs.onFilterChange?.();
      },
    });

    const deleteButton = m(Button, {
      icon: 'close',
      title: 'Delete chart',
      compact: true,
      onclick: (e: MouseEvent) => {
        e.stopPropagation();
        attrs.node.removeChart(config.id);
        attrs.onFilterChange?.();
      },
    });

    const isDragging = this.state.draggingChartId === config.id;
    const isDragOver = this.state.dragOverChartId === config.id;
    const hasCustomWidth = config.widthPx !== undefined;

    const resizeHandle = m(ResizeHandle, {
      direction: 'horizontal',
      onResize: (deltaPx: number) => {
        const currentWidth =
          config.widthPx ??
          this.getDefaultChartWidth(attrs.node.state.chartConfigs.length);
        const newWidth = Math.max(200, currentWidth + deltaPx);
        attrs.node.updateChart(config.id, {widthPx: newWidth});
      },
    });

    return m(
      Card,
      {
        key: config.id,
        style: hasCustomWidth
          ? `--pf-chart-width: ${config.widthPx}px`
          : undefined,
        className: classNames(
          'pf-chart-view__single',
          hasCustomWidth && 'pf-chart-view__single--custom-width',
          isDragging && 'pf-chart-view__single--dragging',
          isDragOver && 'pf-chart-view__single--drag-over',
        ),
        ondragover: (e: DragEvent) => {
          e.preventDefault();
          if (
            this.state.draggingChartId &&
            this.state.draggingChartId !== config.id
          ) {
            this.state.dragOverChartId = config.id;
          }
        },
        ondragleave: () => {
          if (this.state.dragOverChartId === config.id) {
            this.state.dragOverChartId = undefined;
          }
        },
        ondrop: (e: DragEvent) => {
          e.preventDefault();
          if (
            this.state.draggingChartId &&
            this.state.draggingChartId !== config.id
          ) {
            attrs.node.reorderChart(this.state.draggingChartId, config.id);
            attrs.onFilterChange?.();
          }
          this.state.draggingChartId = undefined;
          this.state.dragOverChartId = undefined;
        },
      },
      [
        m(
          '.pf-chart-view__single-header.pf-chart-view__single-header--draggable',
          {
            draggable: true,
            ondragstart: (e: DragEvent) => {
              this.state.draggingChartId = config.id;
              if (e.dataTransfer) {
                e.dataTransfer.effectAllowed = 'move';
              }
            },
            ondragend: () => {
              this.state.draggingChartId = undefined;
              this.state.dragOverChartId = undefined;
            },
          },
          [
            headerTextContent,
            m('.pf-chart-view__single-actions', [
              editButton,
              duplicateButton,
              deleteButton,
            ]),
          ],
        ),
        m('.pf-chart-view__single-content', chartContent),
        resizeHandle,
      ],
    );
  }
}
