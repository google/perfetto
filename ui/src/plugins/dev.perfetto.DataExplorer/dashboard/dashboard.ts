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
import {Accordion, AccordionItem} from '../../../widgets/accordion';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {Chip} from '../../../widgets/chip';
import {EmptyState} from '../../../widgets/empty_state';
import {Icon} from '../../../widgets/icon';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {
  DashboardBrushFilter,
  DashboardDataSource,
  DashboardItem,
  getItemId,
  getLinkedSourceNodeIds,
  getNextItemPosition,
  snapToGrid,
} from './dashboard_registry';
import {
  DashboardChartView,
  createDefaultChartConfig,
} from './dashboard_chart_view';
import {ResizeHandle} from '../../../widgets/resize_handle';
import {Card} from '../../../widgets/card';
import {getDefaultChartLabel} from '../query_builder/nodes/visualisation_node';
import {Popup, PopupPosition} from '../../../widgets/popup';
import {renderChartConfigPopup} from '../query_builder/charts/chart_config_popup';
import {RoundActionButton} from '../query_builder/widgets';

// Default dimensions for dashboard chart cards (in pixels).
const DEFAULT_CHART_WIDTH = 400;
const DEFAULT_CHART_HEIGHT = 294;
const MIN_CHART_WIDTH = 200;
const MIN_CHART_HEIGHT = 150;

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

type SidePanelTab = 'data' | 'linked';

export class Dashboard implements m.ClassComponent<DashboardAttrs> {
  private activePanel?: SidePanelTab;
  private filtersExpanded = true;
  private expandedInput: string | undefined = undefined;
  private editingChartId?: string;

  // Pointer-event drag state.
  private draggingItemId?: string;
  private dragOffset = {x: 0, y: 0};
  private tempPositions = new Map<string, {x: number; y: number}>();
  // Latest attrs — kept in sync every render so drag handlers never use stale
  // references.
  private latestAttrs?: DashboardAttrs;

  view({attrs}: m.CVnode<DashboardAttrs>) {
    this.latestAttrs = attrs;
    const {items} = attrs;

    return m('.pf-dashboard', [
      m('.pf-dashboard__main', [
        this.renderFilterBar(attrs),
        m(
          '.pf-dashboard__canvas',
          {
            onpointermove: (e: PointerEvent) => this.handlePointerMove(e),
            onpointerup: () => this.handlePointerUp(),
            onpointercancel: () => this.cancelDrag(),
          },
          [
            this.renderAddButton(attrs),
            this.renderCollapsedFilterButton(attrs),
            items.length > 0
              ? this.renderItems(attrs, items)
              : m(
                  '.pf-dashboard__empty-overlay',
                  m(
                    EmptyState,
                    {icon: 'bar_chart', title: 'No items yet'},
                    'Use the + button to add charts or labels.',
                  ),
                ),
          ],
        ),
      ]),
      this.activePanel === 'data' &&
        m('.pf-dashboard__data-panel', this.renderDataPanel(attrs)),
      this.activePanel === 'linked' &&
        m('.pf-dashboard__data-panel', this.renderLinkedColumnsPanel(attrs)),
      m('.pf-dashboard__side-panel', [
        m(Button, {
          icon: 'dataset',
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
      ]),
    ]);
  }

  // --- "+" button ---

  private renderAddButton(attrs: DashboardAttrs): m.Child {
    const {sources, items} = attrs;
    // Default to the source of the most recently added chart, or the first
    // available source if there are no charts yet.
    const lastChartItem = [...items].reverse().find((i) => i.kind === 'chart');
    const lastSource =
      (lastChartItem !== undefined
        ? sources.find((s) => s.nodeId === lastChartItem.sourceNodeId)
        : undefined) ?? (sources.length > 0 ? sources[0] : undefined);

    return m(
      '.pf-dashboard__add-button',
      m(
        PopupMenu,
        {
          trigger: RoundActionButton({
            icon: 'add',
            title: 'Add item',
          }),
        },
        m(MenuItem, {
          label: 'Chart',
          icon: 'bar_chart',
          disabled: lastSource === undefined,
          onclick: () => {
            if (lastSource !== undefined) {
              this.addChartForSource(attrs, lastSource);
            }
          },
        }),
        m(MenuItem, {
          label: 'Label',
          icon: 'text_fields',
          onclick: () => {
            this.addLabel(attrs);
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
      renderChartConfigPopup(
        {
          node: new DashboardChartView.Adapter(
            source,
            {...attrs, allSources: attrs.sources},
            config,
          ),
        },
        config,
        () => m.redraw(),
      ),
    );

    const sourceChip = m(
      PopupMenu,
      {
        trigger: m(Chip, {
          label: source.name,
          icon: 'database',
          compact: true,
          className: 'pf-dashboard__source-chip',
          title: 'Change data source',
        }),
      },
      ...attrs.sources.map((s) =>
        m(MenuItem, {
          label: s.name,
          icon: s.nodeId === source.nodeId ? 'check' : undefined,
          onclick: () => {
            if (s.nodeId !== source.nodeId) {
              this.changeChartSource(attrs, itemId, s);
            }
          },
        }),
      ),
    );

    return this.renderItemCard(attrs, itemId, chart, [
      m(
        '.pf-dashboard__chart-header',
        {
          onpointerdown: (e: PointerEvent) => this.startDrag(e, itemId),
        },
        [
          this.editingChartId === itemId
            ? m('input.pf-dashboard__chart-title-input', {
                type: 'text',
                value: config.name ?? '',
                placeholder: getDefaultChartLabel(config),
                oncreate: (vnode: m.VnodeDOM) => {
                  const input = vnode.dom as HTMLInputElement;
                  input.focus();
                  input.select();
                },
                onblur: (e: Event) => {
                  const target = e.target as HTMLInputElement;
                  const name = target.value.trim() || undefined;
                  this.updateChartName(attrs, itemId, name);
                  this.editingChartId = undefined;
                },
                onkeydown: (e: KeyboardEvent) => {
                  if (e.key === 'Enter' || e.key === 'Escape') {
                    this.editingChartId = undefined;
                    (e.target as HTMLInputElement).blur();
                  }
                },
                oninput: (e: Event) => {
                  const target = e.target as HTMLInputElement;
                  const name = target.value || undefined;
                  this.updateChartName(attrs, itemId, name);
                },
              })
            : m(
                '.pf-dashboard__chart-header-text',
                {
                  onclick: (e: MouseEvent) => {
                    e.stopPropagation();
                    this.editingChartId = itemId;
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
        ],
      ),
      m(
        '.pf-dashboard__chart-content',
        m(DashboardChartView, {
          trace: attrs.trace,
          source,
          config,
          dashboardId: attrs.dashboardId,
          items: attrs.items,
          allSources: attrs.sources,
          brushFilters: attrs.brushFilters,
          onItemsChange: attrs.onItemsChange,
          onBrushFiltersChange: attrs.onBrushFiltersChange,
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
      m(
        '.pf-dashboard__chart-header',
        {
          onpointerdown: (e: PointerEvent) => this.startDrag(e, itemId),
        },
        [
          m('.pf-dashboard__chart-header-text', chart.config.name ?? 'Chart'),
          m('.pf-dashboard__chart-actions', [this.deleteButton(attrs, itemId)]),
        ],
      ),
      m(
        '.pf-dashboard__chart-content',
        m(
          EmptyState,
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

  // --- Shared card wrapper with resize + absolute positioning ---

  private renderItemCard(
    attrs: DashboardAttrs,
    itemId: string,
    item: DashboardItem,
    children: m.Children,
  ): m.Child {
    const isDragging = this.draggingItemId === itemId;
    const hasCustomWidth = item.widthPx !== undefined;
    const hasCustomHeight = item.heightPx !== undefined;

    // Use temp position during drag, persisted position otherwise.
    const temp = this.tempPositions.get(itemId);
    const x = temp?.x ?? item.x ?? 0;
    const y = temp?.y ?? item.y ?? 0;

    const styleProps = [
      `left: ${x}px`,
      `top: ${y}px`,
      hasCustomWidth ? `--pf-db-chart-width: ${item.widthPx}px` : '',
      hasCustomHeight ? `--pf-db-chart-height: ${item.heightPx}px` : '',
    ]
      .filter(Boolean)
      .join('; ');

    return m(
      Card,
      {
        key: itemId,
        style: styleProps,
        className: classNames(
          'pf-dashboard__chart',
          hasCustomWidth && 'pf-dashboard__chart--custom-width',
          hasCustomHeight && 'pf-dashboard__chart--custom-height',
          isDragging && 'pf-dashboard__chart--dragging',
        ),
        // Labels are dragged by the card body (not a header).
        onpointerdown:
          item.kind === 'label'
            ? (e: PointerEvent) => {
                if (
                  (e.target as HTMLElement).closest(
                    'textarea, button, input, .pf-resize-handle',
                  )
                ) {
                  return;
                }
                this.startDrag(e, itemId);
              }
            : undefined,
      },
      [
        ...(Array.isArray(children) ? children : [children]),
        m(ResizeHandle, {
          direction: 'horizontal',
          onResize: (deltaPx: number) => {
            const currentWidth = item.widthPx ?? DEFAULT_CHART_WIDTH;
            const newWidth = Math.max(MIN_CHART_WIDTH, currentWidth + deltaPx);
            this.mapItems(attrs, (i) =>
              getItemId(i) === itemId ? {...i, widthPx: newWidth} : i,
            );
          },
        }),
        m(ResizeHandle, {
          direction: 'vertical',
          onResize: (deltaPx: number) => {
            const currentHeight = item.heightPx ?? DEFAULT_CHART_HEIGHT;
            const newHeight = Math.max(
              MIN_CHART_HEIGHT,
              currentHeight + deltaPx,
            );
            this.mapItems(attrs, (i) =>
              getItemId(i) === itemId ? {...i, heightPx: newHeight} : i,
            );
          },
        }),
      ],
    );
  }

  // --- Pointer-event drag ---

  private startDrag(e: PointerEvent, itemId: string): void {
    if (e.button !== 0) return;
    if ((e.target as HTMLElement).closest('button, input, .pf-resize-handle')) {
      return;
    }

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

  private handlePointerMove(e: PointerEvent): void {
    if (this.draggingItemId === undefined) return;

    const canvasEl = e.currentTarget as HTMLElement;
    const canvasRect = canvasEl.getBoundingClientRect();

    const x = Math.max(
      0,
      e.clientX - canvasRect.left + canvasEl.scrollLeft - this.dragOffset.x,
    );
    const y = Math.max(
      0,
      e.clientY - canvasRect.top + canvasEl.scrollTop - this.dragOffset.y,
    );

    this.tempPositions.set(this.draggingItemId, {x, y});
    m.redraw();
  }

  private handlePointerUp(): void {
    if (this.draggingItemId === undefined) return;
    const attrs = this.latestAttrs;
    if (attrs === undefined) return;

    const temp = this.tempPositions.get(this.draggingItemId);
    if (temp !== undefined) {
      const itemId = this.draggingItemId;
      const x = snapToGrid(temp.x);
      const y = snapToGrid(temp.y);
      this.mapItems(attrs, (i) => (getItemId(i) === itemId ? {...i, x, y} : i));
    }
    this.cancelDrag();
  }

  private cancelDrag(): void {
    this.draggingItemId = undefined;
    this.tempPositions.clear();
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
    const {x, y} = getNextItemPosition(items);
    items.push({kind: 'label', id: shortUuid(), text: '', x, y});
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

  // Collapsed filter icon — shown on the canvas when the bar is hidden.
  private renderCollapsedFilterButton(attrs: DashboardAttrs): m.Child {
    if (this.filtersExpanded) return null;
    let count = 0;
    for (const filters of attrs.brushFilters.values()) {
      count += filters.length;
    }
    if (count === 0) return null;
    return m('.pf-dashboard__filter-collapsed', [
      m(Button, {
        icon: 'filter_alt',
        title: `Filters (${count})`,
        onclick: () => {
          this.filtersExpanded = true;
        },
      }),
    ]);
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
    if (!this.filtersExpanded) return null;

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
      const chips = columns.sort().map((col) => {
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
        return m(
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
        );
      });

      groups.push(
        m('.pf-dashboard__filter-group', [
          m(Chip, {
            label,
            icon: 'database',
            compact: true,
            className: 'pf-dashboard__source-chip',
          }),
          ...chips,
        ]),
      );
    }

    return m('.pf-dashboard__filter-bar', [
      ...groups,
      m('.pf-dashboard__filter-bar-actions', [
        m(Button, {
          label: 'Clear all',
          compact: true,
          onclick: () => {
            attrs.onBrushFiltersChange(new Map());
          },
        }),
        m(Button, {
          icon: 'unfold_less',
          compact: true,
          title: 'Collapse filters',
          onclick: () => {
            this.filtersExpanded = false;
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
        EmptyState,
        {icon: 'dataset', title: 'No exported sources'},
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

    const makeSourceItems = (srcs: DashboardDataSource[]): AccordionItem[] =>
      srcs.map((source) => ({
        id: source.nodeId,
        header: m('.pf-dashboard__source-row', [
          m('code.pf-dashboard__input-name', source.name),
        ]),
        content: this.renderInputContent(source, attrs),
      }));

    const sections: m.Child[] = [];
    if (used.length > 0) {
      sections.push(
        m('.pf-dashboard__panel-section', [
          m('.pf-dashboard__panel-section-title', 'Used'),
          m(
            '.pf-dashboard__input-list',
            m(Accordion, {
              items: makeSourceItems(used),
              expanded: this.expandedInput,
              onToggle: (id) => {
                this.expandedInput = id;
              },
            }),
          ),
        ]),
      );
    }
    if (unused.length > 0) {
      sections.push(
        m('.pf-dashboard__panel-section', [
          m('.pf-dashboard__panel-section-title', 'Unused'),
          m(
            '.pf-dashboard__input-list',
            m(Accordion, {
              items: makeSourceItems(unused),
              expanded: this.expandedInput,
              onToggle: (id) => {
                this.expandedInput = id;
              },
            }),
          ),
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
        EmptyState,
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
        EmptyState,
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
                  className: 'pf-dashboard__column-icon',
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
      m(Button, {
        label: 'Add Chart',
        icon: 'bar_chart',
        compact: true,
        className: 'pf-dashboard__add-chart-btn',
        onclick: () => {
          this.addChartForSource(attrs, source);
        },
      }),
    ];
  }

  private addChartForSource(
    attrs: DashboardAttrs,
    source: DashboardDataSource,
  ): void {
    const items = [...attrs.items];
    const newConfig = createDefaultChartConfig(source.columns);
    const {x, y} = getNextItemPosition(items);
    items.push({
      kind: 'chart',
      sourceNodeId: source.nodeId,
      config: newConfig,
      x,
      y,
    });
    attrs.onItemsChange(items);
  }
}
