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
import {Trace} from '../../../public/trace';
import {classNames} from '../../../base/classnames';
import {shortUuid} from '../../../base/uuid';
import {
  perfettoSqlTypeIcon,
  perfettoSqlTypeToString,
} from '../../../trace_processor/perfetto_sql_type';
import {Accordion, AccordionSection} from '../../../widgets/accordion';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {Chip} from '../../../widgets/chip';
import {ResultsPanelEmptyState} from '../query_builder/widgets';
import {Icon} from '../../../widgets/icon';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {
  DashboardBrushFilter,
  DashboardDataSource,
  DashboardItem,
  DEFAULT_COL_SPAN,
  DEFAULT_ROW_SPAN,
  GRID_COLUMNS,
  GRID_MARGIN,
  MIN_COL_SPAN,
  MIN_ROW_SPAN,
  checkOverlap,
  findNonOverlappingPosition,
  getItemBounds,
  getItemId,
  getLinkedSourceNodeIds,
  getNextItemPosition,
  isDriverChart,
} from './dashboard_registry';
import {
  DashboardChartView,
  createDefaultChartConfig,
} from './dashboard_chart_view';
import {ResizeHandle} from '../../../widgets/resize_handle';
import {Card} from '../../../widgets/card';
import {
  ChartType,
  getDefaultChartLabel,
} from '../query_builder/nodes/visualisation_node';
import {Popup, PopupPosition} from '../../../widgets/popup';
import {renderChartConfigPopup} from '../query_builder/charts/chart_config_popup';
import {renderChartTypePickerGrid} from '../query_builder/charts/chart_type_picker';
import {Switch} from '../../../widgets/switch';
import {Select} from '../../../widgets/select';

const DEFAULT_DIVIDER_LABEL = 'Filter boundary';
// CSS selector for elements that should not initiate a card drag.
const DRAG_EXCLUDED_SELECTORS =
  'textarea, button, input, .pf-resize-handle, canvas';
// Delay (ms) after a drag gesture during which title click-to-edit is
// suppressed, so releasing the pointer doesn't accidentally open the editor.
const DRAG_EDIT_SUPPRESS_MS = 300;

function formatBrushFilter(f: DashboardBrushFilter): string {
  if (f.op === 'is null') return 'IS NULL';
  if (f.op === '=') return `= ${f.value}`;
  return `${f.op} ${f.value}`;
}

function formatBrushFilterValue(f: DashboardBrushFilter): string {
  if (f.op === 'is null') return 'NULL';
  if (f.op === '=') return String(f.value);
  return `${f.op} ${f.value}`;
}

function summarizeBrushFilters(
  filters: ReadonlyArray<DashboardBrushFilter>,
): string {
  const eqValues = filters.filter((f) => f.op === '=');
  const rangeFilters = filters.filter((f) => f.op === '>=' || f.op === '<');
  const nullFilters = filters.filter((f) => f.op === 'is null');

  const parts: string[] = [];
  if (rangeFilters.length === 2) {
    const min = rangeFilters.find((f) => f.op === '>=')?.value;
    const max = rangeFilters.find((f) => f.op === '<')?.value;
    if (min !== undefined && max !== undefined) {
      parts.push(`${min}–${max}`);
    }
  } else if (rangeFilters.length > 0) {
    parts.push(...rangeFilters.map((f) => formatBrushFilter(f)));
  }
  if (eqValues.length <= 3) {
    parts.push(...eqValues.map((f) => String(f.value)));
  } else {
    parts.push(`${eqValues.length} values`);
  }
  if (nullFilters.length > 0) {
    parts.push('NULL');
  }
  return parts.join(', ');
}

export interface DashboardAttrs {
  dashboardId: string;
  trace: Trace;
  items: DashboardItem[];
  sources: ReadonlyArray<DashboardDataSource>;
  brushFilters: Map<string, DashboardBrushFilter[]>;
  onItemsChange: (items: DashboardItem[]) => void;
  onBrushFiltersChange: (filters: Map<string, DashboardBrushFilter[]>) => void;
}

type SidePanelTab = 'add' | 'data' | 'linked' | 'edit' | 'settings';

interface DashboardSettings {
  showGridDots: boolean;
  chartGridLines?: 'horizontal' | 'vertical' | 'both';
}

const DEFAULT_SETTINGS: DashboardSettings = {
  showGridDots: true,
  chartGridLines: undefined,
};

interface EditingChartContext {
  readonly itemId: string;
  readonly source: DashboardDataSource;
}

export class Dashboard implements m.ClassComponent<DashboardAttrs> {
  private activePanel?: SidePanelTab;
  private renamingChartId?: string;
  private editingChart?: EditingChartContext;
  private editPanelRenaming = false;
  private settings: DashboardSettings = {...DEFAULT_SETTINGS};
  // Incremented when filters are removed from the filter bar, so that
  // chart brush overlays are cleared in sync.
  private brushClearGen = 0;

  // Grid measurement — updated via ResizeObserver on the canvas element.
  private canvasWidth = 1200;
  private resizeObserver?: ResizeObserver;

  // Pointer-event drag state.
  private draggingItemId?: string;
  private dragOffset = {x: 0, y: 0};
  // Temp positions during drag, stored in grid coordinates (col, row).
  private tempPositions = new Map<string, {col: number; row: number}>();
  // Whether the pointer actually moved during a drag.
  private didMove = false;
  // Timestamp of the last drag end, used to suppress click-to-edit on the
  // title text immediately after a drag gesture.
  private lastDragEndTime = 0;
  // Latest attrs — kept in sync every render so drag handlers never use stale
  // references.
  private latestAttrs?: DashboardAttrs;

  private get cellSize(): number {
    return this.canvasWidth / (GRID_COLUMNS + 2 * GRID_MARGIN);
  }

  view({attrs}: m.CVnode<DashboardAttrs>) {
    this.latestAttrs = attrs;
    const {items} = attrs;

    return m('.pf-dashboard', [
      m('.pf-dashboard__main', [
        this.renderFilterBar(attrs),
        m(
          '.pf-dashboard__canvas',
          {
            className: classNames(
              !this.settings.showGridDots && 'pf-dashboard__canvas--no-dots',
            ),
            style: `--pf-grid-cell-size: ${this.cellSize}px; --pf-grid-margin-px: ${GRID_MARGIN * this.cellSize}px`,
            oncreate: (vnode: m.VnodeDOM) => {
              const el = vnode.dom as HTMLElement;
              this.canvasWidth = el.clientWidth;
              this.resizeObserver = new ResizeObserver((entries) => {
                const newWidth = entries[0]?.contentRect.width;
                if (
                  newWidth !== undefined &&
                  Math.abs(newWidth - this.canvasWidth) > 1
                ) {
                  this.canvasWidth = newWidth;
                  m.redraw();
                }
              });
              this.resizeObserver.observe(el);
            },
            onremove: () => {
              this.resizeObserver?.disconnect();
            },
            onpointermove: (e: PointerEvent) => this.handlePointerMove(e),
            onpointerup: () => this.handlePointerUp(),
            onpointercancel: () => this.endDrag(),
          },
          [
            items.length > 0
              ? [this.renderItems(attrs, items), this.renderCanvasSpacer(items)]
              : m(
                  '.pf-dashboard__empty-overlay',
                  attrs.sources.length > 0
                    ? m(
                        ResultsPanelEmptyState,
                        {icon: 'bar_chart', title: 'No items yet'},
                        'Use the + button in the sidebar to add charts or labels.',
                      )
                    : m(
                        ResultsPanelEmptyState,
                        {icon: 'bar_chart', title: 'No data exported'},
                        'Use the "Export to Dashboard" node in the graph to export data here.',
                      ),
                ),
          ],
        ),
      ]),
      this.activePanel === 'add' &&
        m('.pf-dashboard__content-panel', this.renderAddPanel(attrs)),
      this.activePanel === 'data' &&
        m('.pf-dashboard__content-panel', this.renderDataPanel(attrs)),
      this.activePanel === 'linked' &&
        m('.pf-dashboard__content-panel', this.renderLinkedColumnsPanel(attrs)),
      this.activePanel === 'edit' &&
        this.editingChart !== undefined &&
        m('.pf-dashboard__content-panel', this.renderEditPanel(attrs)),
      this.activePanel === 'settings' &&
        m('.pf-dashboard__content-panel', this.renderSettingsPanel()),
      m('.pf-dashboard__side-panel', [
        m(Button, {
          icon: 'add',
          title: 'Add item',
          className: classNames(this.activePanel === 'add' && 'pf-active'),
          onclick: () => {
            this.activePanel = this.activePanel === 'add' ? undefined : 'add';
          },
        }),
        m(Button, {
          icon: 'storage',
          title: 'Data',
          className: classNames(this.activePanel === 'data' && 'pf-active'),
          onclick: () => {
            this.activePanel = this.activePanel === 'data' ? undefined : 'data';
          },
        }),
        m(Button, {
          icon: 'link',
          title: 'Linked columns',
          className: classNames(this.activePanel === 'linked' && 'pf-active'),
          onclick: () => {
            this.activePanel =
              this.activePanel === 'linked' ? undefined : 'linked';
          },
        }),
        m(Button, {
          icon: 'tune',
          title: 'Dashboard settings',
          className: classNames(this.activePanel === 'settings' && 'pf-active'),
          onclick: () => {
            this.activePanel =
              this.activePanel === 'settings' ? undefined : 'settings';
          },
        }),
        this.activePanel === 'edit' &&
          m(Button, {
            icon: 'settings',
            title: 'Edit chart',
            className: classNames('pf-active'),
            onclick: () => {
              this.activePanel = undefined;
              this.editingChart = undefined;
            },
          }),
      ]),
    ]);
  }

  // --- Add-item side panel ---

  private renderAddPanel(attrs: DashboardAttrs): m.Children {
    const {sources, items} = attrs;
    // Default to the source of the most recently added chart, or the first
    // available source if there are no charts yet.
    const lastChartItem = [...items].reverse().find((i) => i.kind === 'chart');
    const lastSource =
      (lastChartItem !== undefined
        ? sources.find((s) => s.nodeId === lastChartItem.sourceNodeId)
        : undefined) ?? (sources.length > 0 ? sources[0] : undefined);

    return [
      m('.pf-dashboard__add-panel-body', [
        m('.pf-dashboard__panel-section', [
          m('.pf-dashboard__panel-section-title', 'Charts'),
          lastSource !== undefined
            ? renderChartTypePickerGrid((chartType: ChartType) => {
                this.addChartForSource(attrs, lastSource, chartType);
              })
            : m(
                '.pf-dashboard__add-panel-empty',
                'Add a data source first to create charts.',
              ),
        ]),
        m('.pf-dashboard__panel-section', [
          m('.pf-dashboard__panel-section-title', 'Other'),
          m(
            '.pf-dashboard__add-panel-item',
            {
              onclick: () => {
                this.addLabel(attrs);
              },
            },
            [m(Icon, {icon: 'text_fields'}), 'Label'],
          ),
          m(
            '.pf-dashboard__add-panel-item',
            {
              onclick: () => {
                this.addDivider(attrs);
              },
            },
            [m(Icon, {icon: 'horizontal_rule'}), 'Segment Divider'],
          ),
        ]),
      ]),
    ];
  }

  // --- Edit chart side panel ---

  private renderEditPanel(attrs: DashboardAttrs): m.Children {
    const ctx = this.editingChart;
    if (ctx === undefined) return null;

    // Look up the current config from items (it may have been updated).
    const chartItem = attrs.items.find(
      (i) => i.kind === 'chart' && i.config.id === ctx.itemId,
    );
    if (chartItem === undefined || chartItem.kind !== 'chart') {
      // Chart was deleted — close the panel.
      this.activePanel = undefined;
      this.editingChart = undefined;
      return null;
    }
    const config = chartItem.config;
    const source =
      attrs.sources.find((s) => s.nodeId === chartItem.sourceNodeId) ??
      ctx.source;

    const adapter = new DashboardChartView.Adapter(
      source,
      {...attrs, allSources: attrs.sources},
      config,
    );

    const headerLabel = config.name ?? getDefaultChartLabel(config);

    const onChartRemoved = () => {
      // Close panel if the chart was deleted via the form.
      const stillExists = attrs.items.some(
        (i) => i.kind === 'chart' && i.config.id === ctx.itemId,
      );
      if (!stillExists) {
        this.activePanel = undefined;
        this.editingChart = undefined;
      }
    };

    return [
      m('.pf-dashboard__panel-section', [
        this.editPanelRenaming
          ? m('input.pf-dashboard__edit-panel-title-input', {
              type: 'text',
              value: config.name ?? '',
              placeholder: getDefaultChartLabel(config),
              oncreate: (vnode: m.VnodeDOM) => {
                const input = vnode.dom as HTMLInputElement;
                input.focus();
                input.select();
              },
              onblur: () => {
                if (!this.editPanelRenaming) return;
                this.editPanelRenaming = false;
              },
              onkeydown: (e: KeyboardEvent) => {
                if (e.key === 'Escape') {
                  // Revert to the name before editing.
                  this.updateChartName(attrs, config.id, config.name);
                  this.editPanelRenaming = false;
                  (e.target as HTMLInputElement).blur();
                } else if (e.key === 'Enter') {
                  this.editPanelRenaming = false;
                  (e.target as HTMLInputElement).blur();
                }
              },
              oninput: (e: Event) => {
                const target = e.target as HTMLInputElement;
                const name = target.value.trim() || undefined;
                this.updateChartName(attrs, config.id, name);
              },
            })
          : m(
              '.pf-dashboard__edit-panel-title',
              {
                onclick: () => {
                  this.editPanelRenaming = true;
                },
                title: 'Click to rename',
              },
              headerLabel,
            ),
        m('.pf-dashboard__panel-section-subtitle', 'Data Source'),
        m(
          '.pf-dashboard__edit-panel-row',
          this.renderSourceChangePopup(
            attrs,
            config.id,
            source,
            undefined,
            (s) => {
              this.editingChart = {itemId: config.id, source: s};
            },
          ),
        ),
        m('.pf-dashboard__panel-section-subtitle', 'Chart Type'),
        renderChartTypePickerGrid((newType) => {
          if (newType === config.chartType) return;
          const newChartableColumns = adapter.getChartableColumns(newType);
          const columnStillValid = newChartableColumns.some(
            (c) => c.name === config.column,
          );
          adapter.updateChart(config.id, {
            chartType: newType,
            column: columnStillValid ? config.column : '',
          });
        }, config.chartType),
        m('.pf-dashboard__panel-section-subtitle', 'Settings'),
        m(
          '.pf-chart-config-popup',
          renderChartConfigPopup(
            {node: adapter, onFilterChange: onChartRemoved},
            config,
            () => m.redraw(),
            {hideChartTypeSelector: true},
          ),
        ),
      ]),
    ];
  }

  // --- Settings panel ---

  private renderSettingsPanel(): m.Children {
    return [
      m('.pf-dashboard__panel-section', [
        m('.pf-dashboard__panel-section-title', 'Dashboard Settings'),
        m('.pf-dashboard__panel-section-subtitle', 'Canvas'),
        m(
          '.pf-dashboard__settings-row',
          m(Switch, {
            label: 'Show grid dots',
            checked: this.settings.showGridDots,
            onchange: (e: Event) => {
              this.settings.showGridDots = (
                e.target as HTMLInputElement
              ).checked;
            },
          }),
        ),
        m('.pf-dashboard__panel-section-subtitle', 'Charts'),
        m('.pf-dashboard__settings-row', [
          m('label.pf-dashboard__settings-label', 'Grid lines'),
          m(
            Select,
            {
              value: this.settings.chartGridLines ?? 'none',
              onchange: (e: Event) => {
                const val = (e.target as HTMLSelectElement).value;
                this.settings.chartGridLines =
                  val === 'none'
                    ? undefined
                    : (val as 'horizontal' | 'vertical' | 'both');
              },
            },
            [
              m('option', {value: 'none'}, 'None'),
              m('option', {value: 'horizontal'}, 'Horizontal'),
              m('option', {value: 'vertical'}, 'Vertical'),
              m('option', {value: 'both'}, 'Both'),
            ],
          ),
        ]),
      ]),
    ];
  }

  // Invisible spacer that extends 2 grid rows below the lowest item,
  // ensuring there is always room to scroll past the last chart.
  private renderCanvasSpacer(items: DashboardItem[]): m.Child {
    let maxRow = 0;
    for (const item of items) {
      const b = getItemBounds(item);
      maxRow = Math.max(maxRow, b.row + b.rowSpan);
    }
    const cs = this.cellSize;
    const height = (maxRow + 2 + GRID_MARGIN) * cs;
    return m('.pf-dashboard__canvas-spacer', {
      style: `height:${height}px`,
    });
  }

  /** Reusable source-change popup menu for a chart. */
  private renderSourceChangePopup(
    attrs: DashboardAttrs,
    chartId: string,
    currentSource: DashboardDataSource,
    chipOpts?: {compact?: boolean; className?: string},
    onChanged?: (newSource: DashboardDataSource) => void,
  ): m.Child {
    return m(
      PopupMenu,
      {
        trigger: m(Chip, {
          label: currentSource.name,
          icon: 'storage',
          title: 'Change data source',
          ...chipOpts,
        }),
      },
      ...attrs.sources.map((s) =>
        m(MenuItem, {
          label: s.name,
          icon: s.nodeId === currentSource.nodeId ? 'check' : undefined,
          onclick: () => {
            if (s.nodeId !== currentSource.nodeId) {
              this.changeChartSource(attrs, chartId, s);
              onChanged?.(s);
            }
          },
        }),
      ),
    );
  }

  // --- Canvas items ---

  private renderItems(
    attrs: DashboardAttrs,
    items: DashboardItem[],
  ): m.Children {
    return items.map((item) => {
      if (item.kind === 'label') {
        return this.renderLabel(attrs, item);
      }
      if (item.kind === 'divider') {
        return this.renderDivider(attrs, item);
      }
      const source = attrs.sources.find((s) => s.nodeId === item.sourceNodeId);
      if (source === undefined) {
        return this.renderOrphanedChart(attrs, item);
      }
      return this.renderChart(attrs, source, item);
    });
  }

  private renderChart(
    attrs: DashboardAttrs,
    source: DashboardDataSource,
    chart: DashboardItem & {kind: 'chart'},
  ): m.Child {
    const config = chart.config;
    const itemId = config.id;
    const headerLabel = config.name ?? getDefaultChartLabel(config);
    const chartIsDriver = isDriverChart(chart, attrs.items);

    const isEditingThis =
      this.activePanel === 'edit' && this.editingChart?.itemId === itemId;
    const editButton = m(Button, {
      icon: 'settings',
      title: 'Edit chart settings',
      compact: true,
      className: classNames(isEditingThis && 'pf-active'),
      onclick: (e: MouseEvent) => {
        e.stopPropagation();
        if (isEditingThis) {
          this.activePanel = undefined;
          this.editingChart = undefined;
        } else {
          this.editingChart = {itemId, source};
          this.activePanel = 'edit';
          this.editPanelRenaming = false;
        }
      },
    });

    const sourceChip = this.renderSourceChangePopup(attrs, itemId, source, {
      compact: true,
      className: classNames('pf-dashboard__source-chip'),
    });

    return this.renderItemCard(attrs, itemId, chart, [
      m('.pf-dashboard__chart-header', [
        this.renamingChartId === itemId
          ? m('input.pf-dashboard__chart-title-input', {
              type: 'text',
              value: config.name ?? '',
              placeholder: getDefaultChartLabel(config),
              size: Math.max(
                1,
                (config.name ?? getDefaultChartLabel(config)).length,
              ),
              oncreate: (vnode: m.VnodeDOM) => {
                const input = vnode.dom as HTMLInputElement;
                input.focus();
                input.select();
              },
              onblur: (e: Event) => {
                const target = e.target as HTMLInputElement;
                const name = target.value.trim() || undefined;
                this.updateChartName(attrs, itemId, name);
                this.renamingChartId = undefined;
              },
              onkeydown: (e: KeyboardEvent) => {
                if (e.key === 'Enter' || e.key === 'Escape') {
                  this.renamingChartId = undefined;
                  (e.target as HTMLInputElement).blur();
                }
              },
              oninput: (e: Event) => {
                const target = e.target as HTMLInputElement;
                const name = target.value || undefined;
                this.updateChartName(attrs, itemId, name);
                // Update size attribute to match content.
                target.size = Math.max(1, target.value.length);
              },
            })
          : m(
              '.pf-dashboard__chart-header-text',
              {
                onclick: (e: MouseEvent) => {
                  e.stopPropagation();
                  // Don't enter edit mode if we just finished dragging.
                  if (
                    performance.now() - this.lastDragEndTime <
                    DRAG_EDIT_SUPPRESS_MS
                  ) {
                    return;
                  }
                  this.renamingChartId = itemId;
                },
                title: 'Click to rename',
              },
              headerLabel,
            ),
        m('.pf-dashboard__chart-actions', [
          sourceChip,
          editButton,
          this.deleteButton(attrs, itemId),
        ]),
      ]),
      m(
        '.pf-dashboard__chart-content',
        m(DashboardChartView, {
          key: `${config.id}-${this.brushClearGen}`,
          trace: attrs.trace,
          source,
          config,
          dashboardId: attrs.dashboardId,
          items: attrs.items,
          allSources: attrs.sources,
          brushFilters: attrs.brushFilters,
          onItemsChange: attrs.onItemsChange,
          onBrushFiltersChange: attrs.onBrushFiltersChange,
          isDriverChart: chartIsDriver,
          gridLines: this.settings.chartGridLines,
        }),
      ),
    ]);
  }

  private renderOrphanedChart(
    attrs: DashboardAttrs,
    chart: DashboardItem & {kind: 'chart'},
  ): m.Child {
    const itemId = chart.config.id;
    return this.renderItemCard(attrs, itemId, chart, [
      m('.pf-dashboard__chart-header', [
        m('.pf-dashboard__chart-header-text', chart.config.name ?? 'Chart'),
        m('.pf-dashboard__chart-actions', [this.deleteButton(attrs, itemId)]),
      ]),
      m(
        '.pf-dashboard__chart-content',
        m(
          ResultsPanelEmptyState,
          {icon: 'link_off', title: 'Data source removed'},
          'The data source for this chart is no longer available. Delete or re-assign it.',
        ),
      ),
    ]);
  }

  private renderLabel(
    attrs: DashboardAttrs,
    label: DashboardItem & {kind: 'label'},
  ): m.Child {
    const itemId = label.id;

    return this.renderItemCard(attrs, itemId, label, [
      m('.pf-dashboard__label-delete', this.deleteButton(attrs, itemId)),
      m('textarea.pf-dashboard__label-textarea', {
        value: label.text,
        placeholder: 'Type here...',
        oninput: (e: InputEvent) => {
          const target = e.target as HTMLTextAreaElement;
          this.mapItems(attrs, (i) =>
            getItemId(i) === itemId ? {...i, text: target.value} : i,
          );
        },
      }),
    ]);
  }

  private renderDivider(
    attrs: DashboardAttrs,
    divider: DashboardItem & {kind: 'divider'},
  ): m.Child {
    const itemId = divider.id;
    const isDragging = this.draggingItemId === itemId;
    const temp = this.tempPositions.get(itemId);
    const row = temp?.row ?? divider.row;
    const cs = this.cellSize;

    return m(
      '.pf-dashboard__divider',
      {
        key: itemId,
        style: `top: ${(row + GRID_MARGIN) * cs}px`,
        className: classNames(isDragging && 'pf-dashboard__divider--dragging'),
        onpointerdown: (e: PointerEvent) => {
          if (
            (e.target as HTMLElement).closest(
              'button, input, .pf-resize-handle',
            )
          ) {
            return;
          }
          this.startDividerDrag(e, itemId, row);
        },
      },
      [
        m('.pf-dashboard__divider-line'),
        m(
          '.pf-dashboard__divider-label',
          divider.label ?? DEFAULT_DIVIDER_LABEL,
        ),
        m('.pf-dashboard__divider-actions', [
          m(Button, {
            icon: 'close',
            title: 'Remove divider',
            compact: true,
            onclick: (e: MouseEvent) => {
              e.stopPropagation();
              this.removeItem(attrs, itemId);
            },
          }),
        ]),
      ],
    );
  }

  // --- Shared card wrapper with resize + absolute positioning ---

  private renderItemCard(
    attrs: DashboardAttrs,
    itemId: string,
    item: Exclude<DashboardItem, {kind: 'divider'}>,
    children: m.Children,
  ): m.Child {
    const isDragging = this.draggingItemId === itemId;
    const cs = this.cellSize;

    // Use temp position during drag, persisted position otherwise.
    const temp = this.tempPositions.get(itemId);
    const col = temp?.col ?? item.col ?? 0;
    const row = temp?.row ?? item.row ?? 0;
    const colSpan = item.colSpan ?? DEFAULT_COL_SPAN;
    const rowSpan = item.rowSpan ?? DEFAULT_ROW_SPAN;

    const style = [
      `left: ${(col + GRID_MARGIN) * cs}px`,
      `top: ${(row + GRID_MARGIN) * cs}px`,
      `width: ${colSpan * cs}px`,
      `height: ${rowSpan * cs}px`,
    ].join('; ');

    return m(
      Card,
      {
        key: itemId,
        style,
        className: classNames(
          'pf-dashboard__chart',
          isDragging && 'pf-dashboard__chart--dragging',
        ),
        // Allow dragging from anywhere on the card, excluding interactive
        // elements (textareas, buttons, inputs, resize handles, and canvas).
        onpointerdown: (e: PointerEvent) => {
          if ((e.target as HTMLElement).closest(DRAG_EXCLUDED_SELECTORS)) {
            return;
          }
          this.startDrag(e, itemId);
        },
      },
      [
        ...(Array.isArray(children) ? children : [children]),
        m(ResizeHandle, {
          direction: 'horizontal',
          onResizeStart: () => m.redraw(),
          onResizeEnd: () => m.redraw(),
          onResizeAbsolute: (positionPx: number) => {
            const itemCol = item.col ?? 0;
            const itemRow = item.row ?? 0;
            const curRowSpan = item.rowSpan ?? DEFAULT_ROW_SPAN;
            const newSpan = Math.max(
              MIN_COL_SPAN,
              Math.min(GRID_COLUMNS - itemCol, Math.round(positionPx / cs)),
            );
            if (
              newSpan !== (item.colSpan ?? DEFAULT_COL_SPAN) &&
              !checkOverlap(
                itemCol,
                itemRow,
                newSpan,
                curRowSpan,
                attrs.items,
                itemId,
              )
            ) {
              this.mapItems(attrs, (i) =>
                getItemId(i) === itemId ? {...i, colSpan: newSpan} : i,
              );
            }
          },
        }),
        m(ResizeHandle, {
          direction: 'vertical',
          onResizeStart: () => m.redraw(),
          onResizeEnd: () => m.redraw(),
          onResizeAbsolute: (positionPx: number) => {
            const itemCol = item.col ?? 0;
            const itemRow = item.row ?? 0;
            const curColSpan = item.colSpan ?? DEFAULT_COL_SPAN;
            const newSpan = Math.max(MIN_ROW_SPAN, Math.round(positionPx / cs));
            if (
              newSpan !== (item.rowSpan ?? DEFAULT_ROW_SPAN) &&
              !checkOverlap(
                itemCol,
                itemRow,
                curColSpan,
                newSpan,
                attrs.items,
                itemId,
              )
            ) {
              this.mapItems(attrs, (i) =>
                getItemId(i) === itemId ? {...i, rowSpan: newSpan} : i,
              );
            }
          },
        }),
      ],
    );
  }

  // --- Pointer-event drag ---

  private startDrag(e: PointerEvent, itemId: string): void {
    if (e.button !== 0) return;

    const cardEl = (e.currentTarget as HTMLElement).closest(
      '.pf-card',
    ) as HTMLElement | null;
    if (cardEl === null) return;

    const canvasEl = cardEl.closest(
      '.pf-dashboard__canvas',
    ) as HTMLElement | null;
    if (canvasEl === null) return;

    const canvasRect = canvasEl.getBoundingClientRect();
    this.draggingItemId = itemId;
    this.dragOffset = {
      x: e.clientX - canvasRect.left + canvasEl.scrollLeft - cardEl.offsetLeft,
      y: e.clientY - canvasRect.top + canvasEl.scrollTop - cardEl.offsetTop,
    };
  }

  /** Start drag for a divider — only vertical movement matters. */
  private startDividerDrag(
    e: PointerEvent,
    itemId: string,
    currentRow: number,
  ): void {
    if (e.button !== 0) return;

    const canvasEl = (e.currentTarget as HTMLElement).closest(
      '.pf-dashboard__canvas',
    ) as HTMLElement | null;
    if (canvasEl === null) return;

    const canvasRect = canvasEl.getBoundingClientRect();
    this.draggingItemId = itemId;
    this.dragOffset = {
      x: 0,
      y:
        e.clientY -
        canvasRect.top +
        canvasEl.scrollTop -
        currentRow * this.cellSize,
    };
  }

  private handlePointerMove(e: PointerEvent): void {
    if (this.draggingItemId === undefined) return;
    this.didMove = true;

    const canvasEl = e.currentTarget as HTMLElement;
    const canvasRect = canvasEl.getBoundingClientRect();
    const cs = this.cellSize;

    const pixelX = Math.max(
      0,
      e.clientX - canvasRect.left + canvasEl.scrollLeft - this.dragOffset.x,
    );
    const pixelY = Math.max(
      0,
      e.clientY - canvasRect.top + canvasEl.scrollTop - this.dragOffset.y,
    );

    // Snap to grid cells (subtract margin offset before rounding).
    const col = Math.round(pixelX / cs - GRID_MARGIN);
    const row = Math.max(0, Math.round(pixelY / cs - GRID_MARGIN));

    // Dividers only move vertically — keep col at 0.
    const draggingItem = this.latestAttrs?.items.find(
      (i) => getItemId(i) === this.draggingItemId,
    );
    if (draggingItem?.kind === 'divider') {
      this.tempPositions.set(this.draggingItemId, {col: 0, row});
    } else {
      // Clamp col so item doesn't extend past the grid.
      const span =
        draggingItem !== undefined
          ? getItemBounds(draggingItem).colSpan
          : DEFAULT_COL_SPAN;
      const clampedCol = Math.max(0, Math.min(col, GRID_COLUMNS - span));
      this.tempPositions.set(this.draggingItemId, {col: clampedCol, row});
    }
    m.redraw();
  }

  private handlePointerUp(): void {
    if (this.draggingItemId === undefined) return;
    const attrs = this.latestAttrs;
    if (attrs === undefined) return;

    const temp = this.tempPositions.get(this.draggingItemId);
    if (temp !== undefined) {
      const itemId = this.draggingItemId;
      const draggingItem = attrs.items.find((i) => getItemId(i) === itemId);
      const bounds =
        draggingItem !== undefined
          ? getItemBounds(draggingItem)
          : {
              col: 0,
              row: 0,
              colSpan: DEFAULT_COL_SPAN,
              rowSpan: DEFAULT_ROW_SPAN,
            };
      const pos = findNonOverlappingPosition(
        temp.col,
        temp.row,
        bounds.colSpan,
        bounds.rowSpan,
        attrs.items,
        itemId,
      );

      if (draggingItem?.kind === 'divider') {
        this.mapItems(attrs, (i) =>
          getItemId(i) === itemId ? {...i, row: pos.row} : i,
        );
      } else {
        this.mapItems(attrs, (i) =>
          getItemId(i) === itemId ? {...i, col: pos.col, row: pos.row} : i,
        );
      }
    }
    this.endDrag();
  }

  private endDrag(): void {
    this.draggingItemId = undefined;
    this.tempPositions.clear();
    if (this.didMove) {
      this.lastDragEndTime = performance.now();
    }
    this.didMove = false;
    m.redraw();
  }

  private deleteButton(attrs: DashboardAttrs, itemId: string): m.Child {
    return m(Button, {
      icon: 'close',
      title: 'Delete',
      compact: true,
      onclick: (e: MouseEvent) => {
        e.stopPropagation();
        this.removeItem(attrs, itemId);
      },
    });
  }

  // --- Item mutations ---

  private addLabel(attrs: DashboardAttrs): void {
    const items = [...attrs.items];
    const id = shortUuid();
    const candidate = getNextItemPosition(items);
    const pos = findNonOverlappingPosition(
      candidate.col,
      candidate.row,
      DEFAULT_COL_SPAN,
      DEFAULT_ROW_SPAN,
      items,
      id,
    );
    items.push({kind: 'label', id, text: '', col: pos.col, row: pos.row});
    attrs.onItemsChange(items);
  }

  private addDivider(attrs: DashboardAttrs): void {
    const items = [...attrs.items];
    const id = shortUuid();
    // Place the divider below all existing items.
    let maxRow = 0;
    for (const item of items) {
      const b = getItemBounds(item);
      maxRow = Math.max(maxRow, b.row + b.rowSpan);
    }
    const pos = findNonOverlappingPosition(
      0,
      maxRow,
      GRID_COLUMNS,
      1,
      items,
      id,
    );
    items.push({kind: 'divider', id, row: pos.row});
    attrs.onItemsChange(items);
  }

  /** Map over items, replacing those that match a predicate. */
  private mapItems(
    attrs: DashboardAttrs,
    fn: (item: DashboardItem) => DashboardItem,
  ): void {
    attrs.onItemsChange(attrs.items.map(fn));
  }

  private changeChartSource(
    attrs: DashboardAttrs,
    chartId: string,
    newSource: DashboardDataSource,
  ): void {
    this.mapItems(attrs, (i) =>
      i.kind === 'chart' && i.config.id === chartId
        ? {...i, sourceNodeId: newSource.nodeId}
        : i,
    );
  }

  private removeItem(attrs: DashboardAttrs, itemId: string): void {
    attrs.onItemsChange(attrs.items.filter((i) => getItemId(i) !== itemId));
  }

  private updateChartName(
    attrs: DashboardAttrs,
    itemId: string,
    name: string | undefined,
  ): void {
    this.mapItems(attrs, (i) =>
      i.kind === 'chart' && i.config.id === itemId
        ? {...i, config: {...i.config, name}}
        : i,
    );
  }

  private clearBrushFiltersForColumn(
    attrs: DashboardAttrs,
    sourceNodeId: string,
    column: string,
  ): void {
    this.brushClearGen++;
    const newFilters = new Map(attrs.brushFilters);

    // Clear the column from the specified source and all other sources that
    // have a column with the same name (cross-datasource brushing).
    for (const id of getLinkedSourceNodeIds(
      attrs.sources,
      sourceNodeId,
      column,
    )) {
      const current = newFilters.get(id);
      if (current === undefined) continue;
      const filtered = current.filter((f) => f.column !== column);
      if (filtered.length === 0) {
        newFilters.delete(id);
      } else {
        newFilters.set(id, filtered);
      }
    }
    attrs.onBrushFiltersChange(newFilters);
  }

  private removeSingleBrushFilter(
    attrs: DashboardAttrs,
    sourceNodeId: string,
    filter: DashboardBrushFilter,
  ): void {
    this.brushClearGen++;
    const newFilters = new Map(attrs.brushFilters);

    // Remove the filter from the specified source and all other sources that
    // have a column with the same name (cross-datasource brushing).
    for (const id of getLinkedSourceNodeIds(
      attrs.sources,
      sourceNodeId,
      filter.column,
    )) {
      const current = [...(newFilters.get(id) ?? [])];
      const updated = current.filter(
        (f) =>
          f.column !== filter.column ||
          f.op !== filter.op ||
          f.value !== filter.value,
      );
      if (updated.length === 0) {
        newFilters.delete(id);
      } else {
        newFilters.set(id, updated);
      }
    }
    attrs.onBrushFiltersChange(newFilters);
  }

  // --- Filter bar (horizontal strip below tabs) ---

  private renderFilterBar(attrs: DashboardAttrs): m.Child {
    const {sources, brushFilters} = attrs;

    // Collect all active (column, sourceNodeId) filter entries.
    const activeColumns = new Set<string>();
    for (const filters of brushFilters.values()) {
      for (const f of filters) {
        activeColumns.add(f.column);
      }
    }
    if (activeColumns.size === 0) return null;

    // For each active column, compute the sorted set of source nodeIds that
    // share it. The stringified set is the grouping key.
    const sourceNameById = new Map(sources.map((s) => [s.nodeId, s.name]));
    const columnToKey = new Map<string, string>();
    const columnToSourceIds = new Map<string, string[]>();
    for (const col of activeColumns) {
      const ids = getLinkedSourceNodeIds(sources, '', col).sort();
      const key = ids.join(',');
      columnToKey.set(col, key);
      columnToSourceIds.set(col, ids);
    }

    // Group columns by their datasource-set key.
    const groupMap = new Map<
      string,
      {sourceIds: string[]; columns: string[]}
    >();
    for (const col of activeColumns) {
      const key = columnToKey.get(col) ?? '';
      const ids = columnToSourceIds.get(col) ?? [];
      const entry = groupMap.get(key);
      if (entry !== undefined) {
        entry.columns.push(col);
      } else {
        groupMap.set(key, {sourceIds: ids, columns: [col]});
      }
    }

    // Render each group.
    const groups: m.Child[] = [];
    for (const {sourceIds, columns} of groupMap.values()) {
      const label = sourceIds
        .map((id) => sourceNameById.get(id) ?? id)
        .join(', ');
      // Pick any sourceNodeId in the group for clear/remove operations —
      // those functions already propagate across linked sources.
      const representativeSourceId = sourceIds[0];
      const chips = columns.sort().flatMap((col) => {
        // Gather filters for this column from all sources in the group.
        const colFilters: DashboardBrushFilter[] = [];
        for (const id of sourceIds) {
          for (const f of brushFilters.get(id) ?? []) {
            if (f.column === col) colFilters.push(f);
          }
        }
        // De-duplicate (filters are propagated identically across sources).
        const seen = new Set<string>();
        const uniqueFilters = colFilters.filter((f) => {
          const k = `${f.column}|${f.op}|${f.value}`;
          if (seen.has(k)) return false;
          seen.add(k);
          return true;
        });
        // Skip columns with no actual filter values.
        if (uniqueFilters.length === 0) return [];
        return [
          m(
            Popup,
            {
              trigger: m(Chip, {
                label: `${col}: ${summarizeBrushFilters(uniqueFilters)}`,
                icon: 'filter_alt',
                compact: true,
                rounded: true,
                removable: true,
                onRemove: () => {
                  this.clearBrushFiltersForColumn(
                    attrs,
                    representativeSourceId,
                    col,
                  );
                },
              }),
              position: PopupPosition.Bottom,
            },
            m(
              '.pf-dashboard__filter-popup',
              uniqueFilters.map((f) =>
                m(MenuItem, {
                  label: formatBrushFilterValue(f),
                  icon: Icons.Checkbox,
                  onclick: () => {
                    this.removeSingleBrushFilter(
                      attrs,
                      representativeSourceId,
                      f,
                    );
                  },
                }),
              ),
            ),
          ),
        ];
      });

      if (chips.length > 0) {
        groups.push(
          m('.pf-dashboard__filter-group', [
            m(Chip, {
              label,
              icon: 'storage',
              compact: true,
              className: classNames('pf-dashboard__source-chip'),
            }),
            ...chips,
          ]),
        );
      }
    }

    if (groups.length === 0) return null;

    return m('.pf-dashboard__filter-bar', [
      ...groups,
      m('.pf-dashboard__filter-bar-actions', [
        m(Button, {
          label: 'Clear all',
          compact: true,
          onclick: () => {
            this.brushClearGen++;
            attrs.onBrushFiltersChange(new Map());
          },
        }),
      ]),
    ]);
  }

  // --- Data panel ---

  private renderDataPanel(attrs: DashboardAttrs): m.Children {
    const allExported = attrs.sources;

    if (allExported.length === 0) {
      return m(
        ResultsPanelEmptyState,
        {icon: 'storage', title: 'No exported sources'},
        'Use "Export to Dashboard" nodes in the graph builder to make data sources available here.',
      );
    }

    // Partition sources into used (has at least one chart) and unused.
    const items = attrs.items;
    const usedNodeIds = new Set(
      items.filter((i) => i.kind === 'chart').map((i) => i.sourceNodeId),
    );
    const used = allExported.filter((s) => usedNodeIds.has(s.nodeId));
    const unused = allExported.filter((s) => !usedNodeIds.has(s.nodeId));

    const renderSourceAccordion = (srcs: DashboardDataSource[]) =>
      m(
        Accordion,
        srcs.map((source) =>
          m(
            AccordionSection,
            {
              key: source.nodeId,
              summary: m('.pf-dashboard__source-row', [
                m('code.pf-dashboard__input-name', source.name),
              ]),
            },
            this.renderInputContent(source, attrs),
          ),
        ),
      );

    const sections: m.Child[] = [];
    if (used.length > 0) {
      sections.push(
        m('.pf-dashboard__panel-section', [
          m('.pf-dashboard__panel-section-title', 'Used data sources'),
          m('.pf-dashboard__input-list', renderSourceAccordion(used)),
        ]),
      );
    }
    if (unused.length > 0) {
      sections.push(
        m('.pf-dashboard__panel-section', [
          m('.pf-dashboard__panel-section-title', 'Unused data sources'),
          m('.pf-dashboard__input-list', renderSourceAccordion(unused)),
        ]),
      );
    }

    return sections;
  }

  /**
   * Renders the "Linked Columns" panel showing columns that appear in
   * multiple datasources and will be cross-filtered when brushed.
   */
  private renderLinkedColumnsPanel(attrs: DashboardAttrs): m.Children {
    const sources = attrs.sources;
    if (sources.length < 2) {
      return m(
        ResultsPanelEmptyState,
        {icon: 'link', title: 'No linked columns'},
        'Add two or more data sources to see columns that are shared across them.',
      );
    }

    // Build a map: column name → list of sources that contain it.
    const columnSources = new Map<
      string,
      {source: DashboardDataSource; type: string}[]
    >();
    for (const source of sources) {
      for (const col of source.columns) {
        const entries = columnSources.get(col.name) ?? [];
        entries.push({
          source,
          type: perfettoSqlTypeToString(col.type),
        });
        columnSources.set(col.name, entries);
      }
    }

    // Filter to columns that appear in 2+ sources.
    const linked = [...columnSources.entries()]
      .filter(([, entries]) => entries.length >= 2)
      .sort(([a], [b]) => a.localeCompare(b));

    if (linked.length === 0) {
      return m(
        ResultsPanelEmptyState,
        {icon: 'link_off', title: 'No shared columns'},
        'None of the data sources share a column name. Brushing one source will not filter others.',
      );
    }

    return [
      m('.pf-dashboard__panel-section', [
        m(
          '.pf-dashboard__panel-section-title',
          `Linked columns (${linked.length})`,
        ),
        m(
          '.pf-dashboard__input-list',
          linked.map(([columnName, entries]) =>
            m('.pf-dashboard__linked-column', [
              m('.pf-dashboard__linked-column-name', [
                m(Icon, {icon: 'link'}),
                m('code', columnName),
              ]),
              m(
                '.pf-dashboard__linked-column-sources',
                entries.map((entry) =>
                  m('.pf-dashboard__linked-column-source', [
                    m(
                      'span.pf-dashboard__linked-source-name',
                      entry.source.name,
                    ),
                    m('span.pf-dashboard__column-type', entry.type),
                  ]),
                ),
              ),
            ]),
          ),
        ),
      ]),
    ];
  }

  private renderInputContent(
    source: DashboardDataSource,
    attrs: DashboardAttrs,
  ): m.Children {
    return [
      m(
        '.pf-dashboard__detail-row',
        m('span.pf-dashboard__detail-label', 'Columns'),
        m('span.pf-dashboard__detail-value', `${source.columns.length}`),
      ),
      source.columns.length > 0 &&
        m(
          '.pf-dashboard__columns',
          m(
            '.pf-dashboard__column-list',
            source.columns.map((col) =>
              m(
                '.pf-dashboard__column',
                m(Icon, {
                  icon: perfettoSqlTypeIcon(col.type),
                  className: classNames('pf-dashboard__column-icon'),
                }),
                m(
                  '.pf-dashboard__column-info',
                  m(
                    '.pf-dashboard__column-header',
                    m('code.pf-dashboard__column-name', col.name),
                    m(
                      'span.pf-dashboard__column-type',
                      perfettoSqlTypeToString(col.type),
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      m(
        Popup,
        {
          trigger: m(Button, {
            label: 'Add Chart',
            icon: 'bar_chart',
            compact: true,
            className: classNames('pf-dashboard__add-chart-btn'),
          }),
          fitContent: true,
        },
        renderChartTypePickerGrid((chartType: ChartType) => {
          this.addChartForSource(attrs, source, chartType);
        }),
      ),
    ];
  }

  private addChartForSource(
    attrs: DashboardAttrs,
    source: DashboardDataSource,
    chartType: ChartType,
  ): void {
    const items = [...attrs.items];
    const newConfig = createDefaultChartConfig(source.columns, chartType);
    const candidate = getNextItemPosition(items);
    const pos = findNonOverlappingPosition(
      candidate.col,
      candidate.row,
      DEFAULT_COL_SPAN,
      DEFAULT_ROW_SPAN,
      items,
      newConfig.id,
    );
    items.push({
      kind: 'chart',
      sourceNodeId: source.nodeId,
      config: newConfig,
      col: pos.col,
      row: pos.row,
    });
    attrs.onItemsChange(items);

    // Automatically open the edit panel for the newly added chart.
    this.editingChart = {itemId: newConfig.id, source};
    this.activePanel = 'edit';
  }
}
