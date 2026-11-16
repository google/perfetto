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
import {copyToClipboard} from '../../base/clipboard';
import {Icons} from '../../base/semantic_icons';
import {Duration, Time, type time} from '../../base/time';
import {formatDurationValue} from '../../components/aggregation_panel';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {
  AggregateColumn,
  Pivot,
} from '../../components/widgets/datagrid/model';
import type {
  ColumnSchema,
  ColumnType,
} from '../../components/widgets/datagrid/datagrid_schema';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {Selection, TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {SqlValue} from '../../trace_processor/query_result';
import {sqlValueToReadableString} from '../../trace_processor/sql_utils';
import {Button, ButtonVariant} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {DetailsShell} from '../../widgets/details_shell';
import {EmptyState} from '../../widgets/empty_state';
import {Keycap} from '../../widgets/hotkey_glyphs';
import {LinearProgress} from '../../widgets/linear_progress';
import {MenuDivider, MenuItem, MenuTitle, PopupMenu} from '../../widgets/menu';
import {Section} from '../../widgets/section';
import {Select} from '../../widgets/select';
import {SqlRef} from '../../widgets/sql_ref';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  DrawerPanel,
  DrawerPanelVisibility,
  type DrawerTab,
} from '../../widgets/drawer_panel';
import type {
  BrushState,
  PointRef,
  ScatterController,
} from './scatter_controller';
import {ScatterPlotView} from './scatter_plot_view';
import {SourcePicker} from './source_picker';
import type {DataBounds, PointStyle} from './types';
import {fitView} from './view';

export interface ScatterPageAttrs {
  readonly trace: Trace;
  readonly controller: ScatterController;
}

const COMMON_TABLE_SUGGESTIONS = ['slice', 'thread_state', 'sched', 'counter'];
const DEFAULT_DRAWER_HEIGHT = 280;

export class ScatterPage implements m.ClassComponent<ScatterPageAttrs> {
  private static savedDrawerVisibility = DrawerPanelVisibility.COLLAPSED;
  private static savedTabKey = 'current_selection';

  private pointStyle: PointStyle = {
    sizePx: 3,
    opacity: 0.7,
  };

  private get drawerVisibility(): DrawerPanelVisibility {
    return ScatterPage.savedDrawerVisibility;
  }
  private set drawerVisibility(v: DrawerPanelVisibility) {
    ScatterPage.savedDrawerVisibility = v;
  }

  private get activeTabKey(): string {
    return ScatterPage.savedTabKey;
  }
  private set activeTabKey(k: string) {
    ScatterPage.savedTabKey = k;
  }

  private lastPinned?: PointRef;
  private lastBrush?: BrushState;

  private brushDataSource?: SQLDataSource;
  private lastBrushRect?: DataBounds;
  private activeTimelinePanel?: TrackEventDetailsPanel;
  private panelToken = 0;
  private panelLoading = false;
  private lastResolvedId?: number;
  private lastResolvedTable?: string;
  private lastObservedSelection?: Selection;
  private isDisposed = false;

  oninit() {
    this.isDisposed = false;
  }

  onremove() {
    this.isDisposed = true;
    this.disposeBrushDataSource();
    this.clearPinnedPanel();
  }

  private disposeBrushDataSource(): void {
    if (this.brushDataSource !== undefined) {
      try {
        this.brushDataSource.dispose();
      } catch {
        // Ignored.
      }
      this.brushDataSource = undefined;
      this.lastBrushRect = undefined;
    }
  }

  private clearPinnedPanel(): void {
    this.panelToken++;
    this.activeTimelinePanel = undefined;
    this.panelLoading = false;
    this.lastResolvedTable = undefined;
    this.lastResolvedId = undefined;
  }

  private loadTimelinePanel(
    trace: Trace,
    tableName: string,
    targetId: number,
  ): void {
    this.lastResolvedTable = tableName;
    this.lastResolvedId = targetId;
    this.panelLoading = true;
    this.activeTimelinePanel = undefined;
    const token = ++this.panelToken;

    void (async () => {
      try {
        const events = await trace.selection.resolveSqlEvents(tableName, [
          targetId,
        ]);
        if (this.isDisposed || token !== this.panelToken) return;
        if (events.length > 0) {
          const ev = events[0];
          const track = trace.tracks.getTrack(ev.trackUri);
          const d = await track?.renderer.getSelectionDetails?.(ev.eventId);
          if (this.isDisposed || token !== this.panelToken) return;
          const sel: TrackEventSelection = {
            ...d,
            ts: d?.ts ?? Time.fromRaw(0n),
            kind: 'track_event',
            trackUri: ev.trackUri,
            eventId: ev.eventId,
          };
          const panel = track?.renderer.detailsPanel?.(sel);
          if (panel !== undefined) {
            await panel.load?.(sel);
            if (this.isDisposed || token !== this.panelToken) return;
            this.activeTimelinePanel = panel;
            this.panelLoading = false;
            trace.raf.scheduleFullRedraw();
            return;
          }
        }
      } catch {
        // Fallback to generic details
      }
      if (this.isDisposed || token !== this.panelToken) return;
      this.activeTimelinePanel = undefined;
      this.panelLoading = false;
      trace.raf.scheduleFullRedraw();
    })();
  }

  view({attrs}: m.CVnode<ScatterPageAttrs>) {
    const {trace, controller} = attrs;
    const state = controller.state;

    // Navigate to timeline if selection was changed by user actions in the details panel
    const curSelection = trace.selection.selection;
    if (
      this.lastObservedSelection !== undefined &&
      this.lastObservedSelection !== curSelection &&
      curSelection.kind !== 'empty'
    ) {
      this.lastObservedSelection = curSelection;
      trace.navigate('#!/viewer');
    } else {
      this.lastObservedSelection = curSelection;
    }

    // Auto-open drawer when point pinned or brush selection made
    if (controller.pinned !== this.lastPinned) {
      this.lastPinned = controller.pinned;
      if (controller.pinned !== undefined) {
        this.drawerVisibility = DrawerPanelVisibility.VISIBLE;
        this.activeTabKey = 'current_selection';
      } else {
        this.clearPinnedPanel();
      }
    }
    if (controller.brush !== this.lastBrush) {
      this.lastBrush = controller.brush;
      if (controller.brush !== undefined) {
        this.drawerVisibility = DrawerPanelVisibility.VISIBLE;
        this.activeTabKey = 'selection';
      } else {
        this.disposeBrushDataSource();
      }
    }

    let brushTabTitle = 'Selection';
    if (controller.source?.kind === 'table') {
      if (controller.source.table === 'slice') {
        brushTabTitle = 'Slices';
      } else if (controller.source.table === 'thread_state') {
        brushTabTitle = 'Thread States';
      } else if (controller.source.table === 'sched') {
        brushTabTitle = 'CPU by Thread';
      }
    }

    const tabs: DrawerTab[] = [
      {
        key: 'current_selection',
        title: 'Current Selection',
        content: this.renderPinnedTab(controller, trace),
        closable: controller.pinned !== undefined,
      },
      {
        key: 'selection',
        title: brushTabTitle,
        content: this.renderSelectionTab(controller, trace),
        closable: controller.brush !== undefined,
      },
    ];

    return m(
      '.pf-scatter-page',
      this.renderToolbar(controller, trace),
      this.renderProgressBar(controller),
      m(
        '.pf-scatter-page__main',
        state.kind === 'empty'
          ? this.renderEmptyState(controller)
          : m(
              '.pf-scatter-page__content',
              // The plot always fills the whole area; the drawer floats on
              // top of it so opening/resizing the drawer never resizes the
              // plot.
              m(ScatterPlotView, {
                controller,
                pointStyle: this.pointStyle,
              }),
              m(DrawerPanel, {
                className: 'pf-scatter-page__drawer',
                visibility: this.drawerVisibility,
                onVisibilityChange: (visibility: DrawerPanelVisibility) => {
                  this.drawerVisibility = visibility;
                },
                activeTabKey: this.activeTabKey,
                onTabChange: (key: string) => {
                  this.activeTabKey = key;
                  if (
                    this.drawerVisibility === DrawerPanelVisibility.COLLAPSED
                  ) {
                    this.drawerVisibility = DrawerPanelVisibility.VISIBLE;
                  }
                },
                onTabClose: (key: string) => {
                  if (key === 'current_selection') {
                    controller.pin(undefined);
                  } else if (key === 'selection') {
                    controller.setBrush(undefined);
                  }
                },
                startingHeight: DEFAULT_DRAWER_HEIGHT,
                leftHandleContent: this.renderDrawerMenu(
                  controller,
                  brushTabTitle,
                ),
                mainContent: this.renderStateCallout(controller),
                tabs,
              }),
            ),
      ),
    );
  }

  private renderStateCallout(controller: ScatterController): m.Children {
    const state = controller.state;
    let message: string | undefined;
    let intent = Intent.None;
    if (state.kind === 'error') {
      message = state.message;
      intent = Intent.Danger;
    } else if (state.kind === 'needs_axes') {
      message =
        'This source has fewer than two numeric columns (or no rows), so ' +
        'there is nothing to plot. Pick another table or write a query.';
      intent = Intent.Warning;
    } else if (state.kind === 'ready' && controller.rowCount === 0) {
      message = 'No rows with non-NULL values for the selected X and Y.';
      intent = Intent.Warning;
    }
    if (message === undefined) return null;
    return m(Callout, {intent, className: 'pf-scatter-page__error'}, message);
  }

  private renderProgressBar(controller: ScatterController): m.Children {
    const state = controller.state;
    const isLoading =
      state.kind === 'loading' ||
      state.kind === 'preparing' ||
      state.kind === 'describing';

    return m(LinearProgress, {
      className: 'pf-scatter-page__progress',
      state: isLoading ? 'indeterminate' : 'none',
    });
  }

  private renderToolbar(
    controller: ScatterController,
    trace: Trace,
  ): m.Children {
    const source = controller.source;
    let sourceLabel = 'Select table / query...';
    if (source?.kind === 'table') {
      sourceLabel = `Table: ${source.table}`;
    } else if (source?.kind === 'query') {
      sourceLabel = 'Custom SQL query';
    }

    const numericCols = controller.columns.filter((c) => c.kind === 'numeric');
    const plot = controller.plot;
    const state = controller.state;

    return m(
      '.pf-scatter-page__toolbar',
      // Source picker button
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: sourceLabel,
            icon: 'table_chart',
            rightIcon: 'arrow_drop_down',
          }),
        },
        m(SourcePicker, {
          trace,
          currentSource: controller.source,
          onSelect: (spec) => {
            void controller.setSource(spec);
          },
        }),
      ),

      // X Axis selector
      m(
        '.pf-scatter-page__control-group',
        m('.pf-scatter-page__control-label', 'X:'),
        m(
          Select,
          {
            value: plot?.x ?? '',
            disabled: numericCols.length === 0,
            onchange: (e: Event) => {
              const val = (e.target as HTMLSelectElement).value;
              if (plot !== undefined && val.length > 0) {
                void controller.setPlot({...plot, x: val});
              }
            },
          },
          numericCols.map((c) =>
            m('option', {value: c.name, selected: c.name === plot?.x}, c.name),
          ),
        ),
      ),

      // Y Axis selector
      m(
        '.pf-scatter-page__control-group',
        m('.pf-scatter-page__control-label', 'Y:'),
        m(
          Select,
          {
            value: plot?.y ?? '',
            disabled: numericCols.length === 0,
            onchange: (e: Event) => {
              const val = (e.target as HTMLSelectElement).value;
              if (plot !== undefined && val.length > 0) {
                void controller.setPlot({...plot, y: val});
              }
            },
          },
          numericCols.map((c) =>
            m('option', {value: c.name, selected: c.name === plot?.y}, c.name),
          ),
        ),
      ),

      // Color selector
      m(
        '.pf-scatter-page__control-group',
        m('.pf-scatter-page__control-label', 'Color:'),
        m(
          Select,
          {
            value: plot?.color ?? '',
            disabled: controller.columns.length === 0,
            onchange: (e: Event) => {
              const val = (e.target as HTMLSelectElement).value;
              if (plot !== undefined) {
                void controller.setPlot({
                  ...plot,
                  color: val.length > 0 ? val : undefined,
                });
              }
            },
          },
          m('option', {value: ''}, 'None'),
          controller.columns.map((c) =>
            m(
              'option',
              {value: c.name, selected: c.name === plot?.color},
              c.name,
            ),
          ),
        ),
      ),

      // Reset view button
      m(Button, {
        label: 'Reset view',
        icon: 'fit_screen',
        variant: ButtonVariant.Outlined,
        disabled: controller.view === undefined,
        onclick: () => controller.resetView(),
      }),

      // Settings menu
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            icon: 'settings',
            title: 'Settings',
            variant: ButtonVariant.Minimal,
          }),
        },
        m(MenuTitle, {label: 'Point Size'}),
        [1, 2, 3, 4, 6].map((sz) =>
          m(MenuItem, {
            label: `${sz} px`,
            active: this.pointStyle.sizePx === sz,
            onclick: () => {
              this.pointStyle = {...this.pointStyle, sizePx: sz};
            },
          }),
        ),
        m(MenuDivider),
        m(MenuTitle, {label: 'Opacity'}),
        [0.1, 0.25, 0.5, 0.75, 1.0].map((op) =>
          m(MenuItem, {
            label: `${Math.round(op * 100)}%`,
            active: this.pointStyle.opacity === op,
            onclick: () => {
              this.pointStyle = {...this.pointStyle, opacity: op};
            },
          }),
        ),
      ),

      // Help shortcuts menu
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            icon: 'help',
            title: 'Keyboard & mouse shortcuts',
            variant: ButtonVariant.Minimal,
          }),
        },
        this.renderHelpMenu(),
      ),

      // Status text
      m(
        '.pf-scatter-page__status',
        state.kind === 'loading' &&
          (state.total !== undefined
            ? `Loading ${state.total.toLocaleString()} rows...`
            : 'Loading...'),
        state.kind === 'ready' &&
          (() => {
            const parts: string[] = [];
            if (controller.rowCount !== undefined) {
              parts.push(`${controller.rowCount.toLocaleString()} rows`);
            }
            if (controller.lastResultRows !== undefined) {
              parts.push(
                `${controller.lastResultRows.toLocaleString()} points in view`,
              );
            }
            if (controller.lastQueryMs !== undefined) {
              parts.push(`${controller.lastQueryMs} ms`);
            }
            let text = parts.join(' · ');
            if (controller.isUpdating) {
              text += ' (updating...)';
            }
            return text;
          })(),
      ),
    );
  }

  private renderEmptyState(controller: ScatterController): m.Children {
    return m(
      '.pf-scatter-page__empty-container',
      m(
        EmptyState,
        {
          icon: 'scatter_plot',
          title: 'Scatter Plot',
          fillHeight: false,
        },
        m(
          'p',
          'Explore millions of trace events with hardware-accelerated pan and zoom.',
        ),
        m(
          '.pf-scatter-page__suggestions',
          COMMON_TABLE_SUGGESTIONS.map((tbl) =>
            m(Button, {
              label: `Plot ${tbl}`,
              variant: ButtonVariant.Outlined,
              onclick: () => {
                void controller.setSource({kind: 'table', table: tbl});
              },
            }),
          ),
        ),
      ),
    );
  }

  private renderPinnedTab(
    controller: ScatterController,
    trace: Trace,
  ): m.Children {
    const pinned = controller.pinned;
    const details = controller.pinnedDetails;

    if (pinned === undefined) {
      this.clearPinnedPanel();
      return m(
        '.pf-scatter-drawer__empty',
        'Click any point in the plot to pin and view its attributes.',
      );
    }

    const cells = details?.cells ?? [];
    let tsVal: number | bigint | undefined;
    let durVal: number | bigint | undefined;
    let idVal: number | bigint | undefined;

    for (const cell of cells) {
      if (
        cell.name === 'id' &&
        (typeof cell.value === 'number' || typeof cell.value === 'bigint')
      ) {
        idVal = cell.value;
      }
      if (
        (cell.name === 'ts' || cell.name.endsWith('_ts')) &&
        (typeof cell.value === 'number' || typeof cell.value === 'bigint')
      ) {
        tsVal = cell.value;
      }
      if (
        (cell.name === 'dur' || cell.name.endsWith('_dur')) &&
        (typeof cell.value === 'number' || typeof cell.value === 'bigint')
      ) {
        durVal = cell.value;
      }
    }

    const isTableSource = controller.source?.kind === 'table';
    const tableName = isTableSource ? controller.source.table : undefined;
    const targetId = idVal !== undefined ? Number(idVal) : undefined;

    // Trigger async load of timeline details panel when table and id exist
    if (tableName !== undefined && targetId !== undefined) {
      if (
        tableName !== this.lastResolvedTable ||
        targetId !== this.lastResolvedId
      ) {
        this.loadTimelinePanel(trace, tableName, targetId);
      }
    }

    const renderActions = () =>
      m(
        '.pf-scatter-drawer__actions',
        tsVal !== undefined &&
          m(Button, {
            label: 'Go to timeline',
            icon: 'schedule',
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            onclick: () => {
              const start = Time.fromRaw(BigInt(tsVal));
              let end: time | undefined;
              if (durVal !== undefined && durVal > 0n) {
                end = Time.fromRaw(BigInt(tsVal) + BigInt(durVal));
              }
              trace.scrollTo({
                time: {
                  start,
                  end,
                  behavior: 'focus',
                },
              });
              trace.navigate('#!/viewer');
            },
          }),
        isTableSource &&
          targetId !== undefined &&
          m(Button, {
            label: 'Select in timeline',
            icon: 'target',
            variant: ButtonVariant.Outlined,
            onclick: () => {
              if (controller.source?.kind === 'table') {
                try {
                  trace.selection.selectSqlEvent(
                    controller.source.table,
                    targetId,
                    {
                      switchToCurrentSelectionTab: true,
                      scrollToSelection: true,
                    },
                  );
                  trace.navigate('#!/viewer');
                } catch {
                  // Fallback
                }
              }
            },
          }),
        m(Button, {
          icon: 'close',
          variant: ButtonVariant.Minimal,
          title: 'Unpin point',
          onclick: () => controller.pin(undefined),
        }),
      );

    // If the real timeline details panel was resolved, render it
    if (this.activeTimelinePanel !== undefined) {
      return m(
        '.pf-scatter-drawer__pane.pf-scatter-drawer__pane--details',
        m(
          '.pf-scatter-drawer__details-toolbar',
          m('.pf-scatter-drawer__title', 'Current Selection'),
          renderActions(),
        ),
        this.panelLoading && m(LinearProgress),
        m('.pf-scatter-drawer__real-panel', this.activeTimelinePanel.render()),
      );
    }

    // Fallback: Tree-based generic details panel with formatted widgets
    return m(
      '.pf-scatter-drawer__pane.pf-scatter-drawer__pane--details',
      (details === undefined || this.panelLoading) && m(LinearProgress),
      cells.length > 0
        ? m(
            '.pf-scatter-drawer__real-panel',
            m(
              DetailsShell,
              {
                title: 'Point details',
                buttons: renderActions(),
              },
              m(
                Section,
                {title: 'Attributes'},
                m(
                  Tree,
                  cells.map((cell) => {
                    const valStr =
                      cell.value === null ? 'NULL' : String(cell.value);
                    let rightWidget: m.Children;
                    if (
                      cell.name === 'id' &&
                      isTableSource &&
                      targetId !== undefined
                    ) {
                      rightWidget = m(SqlRef, {
                        table: controller.source!.table,
                        id: targetId,
                      });
                    } else if (
                      (cell.name === 'ts' || cell.name.endsWith('_ts')) &&
                      (typeof cell.value === 'number' ||
                        typeof cell.value === 'bigint')
                    ) {
                      rightWidget = m(Timestamp, {
                        trace,
                        ts: Time.fromRaw(
                          BigInt(Math.round(Number(cell.value))),
                        ),
                      });
                    } else if (
                      (cell.name === 'dur' || cell.name.endsWith('_dur')) &&
                      (typeof cell.value === 'number' ||
                        typeof cell.value === 'bigint')
                    ) {
                      rightWidget = m(DurationWidget, {
                        trace,
                        dur: Duration.fromRaw(
                          BigInt(Math.round(Number(cell.value))),
                        ),
                      });
                    } else {
                      rightWidget = m(
                        'span.pf-scatter-drawer__cell-val',
                        {
                          title: `${valStr} (click to copy)`,
                          onclick: () => {
                            copyToClipboard(valStr);
                          },
                        },
                        sqlValueToReadableString(cell.value),
                      );
                    }
                    return m(TreeNode, {
                      left: cell.name,
                      right: rightWidget,
                    });
                  }),
                ),
              ),
            ),
          )
        : m(
            '.pf-scatter-drawer__details-toolbar',
            m('.pf-scatter-drawer__title', 'Point details'),
            renderActions(),
          ),
    );
  }

  private renderSelectionTab(
    controller: ScatterController,
    trace: Trace,
  ): m.Children {
    const brush = controller.brush;
    if (brush === undefined) {
      this.disposeBrushDataSource();
      return m(
        '.pf-scatter-drawer__empty',
        'Shift + Drag on the plot to select a rectangular region.',
      );
    }

    const countText =
      brush.count !== undefined
        ? `${brush.count.toLocaleString()} rows selected`
        : 'Counting selected rows...';

    // Memoize and create brush SQLDataSource
    const r = brush.rect;
    if (
      this.brushDataSource === undefined ||
      this.lastBrushRect === undefined ||
      this.lastBrushRect.xMin !== r.xMin ||
      this.lastBrushRect.xMax !== r.xMax ||
      this.lastBrushRect.yMin !== r.yMin ||
      this.lastBrushRect.yMax !== r.yMax
    ) {
      this.disposeBrushDataSource();
      const rawSql = controller.sqlForBrush();
      if (rawSql !== undefined) {
        const cleanSql = rawSql.trim().replace(/;+$/, '');
        this.brushDataSource = new SQLDataSource({
          engine: trace.engine,
          tableOrSubquery: cleanSql,
        });
        this.lastBrushRect = {...r};
      }
    }

    // Build ColumnSchema from controller columns
    const schema: ColumnSchema = {};
    for (const col of controller.columns) {
      let colType: ColumnType = 'text';
      if (col.kind === 'numeric') {
        colType = col.name === 'id' ? 'identifier' : 'quantitative';
      }
      const isTs = col.name === 'ts' || col.name.endsWith('_ts');
      const isDur = col.name === 'dur' || col.name.endsWith('_dur');

      schema[col.name] = {
        title: col.name,
        columnType: colType,
        cellRenderer: isTs
          ? (val: SqlValue) => {
              if (typeof val === 'bigint') {
                return m(Timestamp, {trace, ts: Time.fromRaw(val)});
              } else if (typeof val === 'number') {
                return m(Timestamp, {
                  trace,
                  ts: Time.fromRaw(BigInt(Math.round(val))),
                });
              }
              return String(val ?? '');
            }
          : isDur
            ? (val: SqlValue) => {
                if (typeof val === 'bigint') {
                  return formatDurationValue(val);
                } else if (typeof val === 'number') {
                  return formatDurationValue(BigInt(Math.round(val)));
                }
                return String(val ?? '');
              }
            : undefined,
      };
    }

    const initialColumns = controller.columns.map((c) => ({
      id: c.name,
      field: c.name,
    }));

    const hasNameCol = controller.columns.some((c) => c.name === 'name');
    const hasDurCol = controller.columns.some((c) => c.name === 'dur');

    let initialPivot: Pivot | undefined = undefined;
    if (hasNameCol) {
      const aggregates: AggregateColumn[] = [{id: 'count', function: 'COUNT'}];
      if (hasDurCol) {
        aggregates.push(
          {id: 'dur_sum', field: 'dur', function: 'SUM', sort: 'DESC'},
          {id: 'dur_avg', field: 'dur', function: 'AVG'},
        );
      }
      initialPivot = {
        groupBy: [{id: 'name', field: 'name'}],
        aggregates,
      };
    }

    const leftToolbar = m(
      '.pf-scatter-drawer__toolbar-left',
      m('.pf-scatter-drawer__title', 'Box Selection'),
      m('.pf-scatter-drawer__count', countText),
    );

    const rightToolbar = m(
      '.pf-scatter-drawer__action-row',
      m(Button, {
        label: 'Zoom to selection',
        icon: 'zoom_in',
        compact: true,
        variant: ButtonVariant.Outlined,
        onclick: () => {
          controller.setView(fitView(brush.rect));
        },
      }),
      m(CopyToClipboardButton, {
        label: 'Copy SQL',
        compact: true,
        variant: ButtonVariant.Outlined,
        textToCopy: () => controller.sqlForBrush() ?? '',
      }),
      m(Button, {
        icon: Icons.Close,
        compact: true,
        variant: ButtonVariant.Minimal,
        title: 'Clear selection',
        onclick: () => controller.setBrush(undefined),
      }),
    );

    return m(
      '.pf-scatter-drawer__pane.pf-scatter-drawer__pane--selection',
      this.brushDataSource === undefined &&
        m('.pf-scatter-drawer__selection-header', leftToolbar, rightToolbar),
      m(
        '.pf-scatter-drawer__grid-container',
        this.brushDataSource !== undefined &&
          m(DataGrid, {
            fillHeight: true,
            schema,
            data: this.brushDataSource,
            initialColumns,
            initialPivot,
            showExportButton: true,
            toolbarItemsLeft: leftToolbar,
            toolbarItemsRight: rightToolbar,
          }),
      ),
    );
  }

  private renderHelpMenu(): m.Children {
    const shortcuts = [
      {key: 'Drag', desc: 'Pan view'},
      {key: 'Shift + Drag', desc: 'Box select'},
      {key: 'Wheel', desc: 'Zoom at cursor'},
      {key: 'Shift + Wheel', desc: 'Zoom X only'},
      {key: 'Alt + Wheel', desc: 'Zoom Y only'},
      {key: 'W / S, + / −', desc: 'Zoom in / out at cursor'},
      {key: 'A / D', desc: 'Pan left / right'},
      {key: 'Arrows', desc: 'Pan in any direction'},
      {key: 'Double click', desc: 'Reset view'},
      {key: 'Escape', desc: 'Clear selection / pin'},
      {key: 'Legend click', desc: 'Hide / show category'},
      {key: 'Legend double click', desc: 'Show only that category'},
    ];

    return m(
      '.pf-scatter-help-menu',
      m(MenuTitle, {label: 'Shortcuts'}),
      m(
        '.pf-scatter-help-menu__list',
        shortcuts.map((s) =>
          m(
            '.pf-scatter-help-menu__item',
            m(Keycap, s.key),
            m('span.pf-scatter-help-menu__desc', s.desc),
          ),
        ),
      ),
    );
  }

  private renderDrawerMenu(
    _controller: ScatterController,
    brushTitle: string,
  ): m.Child {
    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          icon: Icons.ContextMenuAlt,
          title: 'Drawer Options',
        }),
      },
      m(MenuItem, {
        label: 'Current Selection',
        icon:
          this.activeTabKey === 'current_selection'
            ? Icons.Checkbox
            : Icons.BlankCheckbox,
        onclick: () => {
          this.activeTabKey = 'current_selection';
          if (this.drawerVisibility === DrawerPanelVisibility.COLLAPSED) {
            this.drawerVisibility = DrawerPanelVisibility.VISIBLE;
          }
        },
      }),
      m(MenuItem, {
        label: brushTitle,
        icon:
          this.activeTabKey === 'selection'
            ? Icons.Checkbox
            : Icons.BlankCheckbox,
        onclick: () => {
          this.activeTabKey = 'selection';
          if (this.drawerVisibility === DrawerPanelVisibility.COLLAPSED) {
            this.drawerVisibility = DrawerPanelVisibility.VISIBLE;
          }
        },
      }),
    );
  }
}
