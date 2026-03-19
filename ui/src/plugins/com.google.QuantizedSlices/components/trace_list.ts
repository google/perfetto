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
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {SplitPanel} from '../../../widgets/split_panel';
import {Popup, PopupPosition} from '../../../widgets/popup';
import {Menu, MenuItem, MenuDivider} from '../../../widgets/menu';
import {Button, ButtonVariant} from '../../../widgets/button';
import {
  S,
  activeCluster,
  filteredTraces,
  filterTraces,
  updateGlobalSlider,
  getFilterableFields,
  getFieldValues,
  togglePropFilter,
  clearPropFilter,
  copyFilteredToNewTab,
  getCrossCompareState,
  startCrossCompare,
} from '../state';
import type {TraceState, Cluster} from '../state';
import type {OverviewFilter} from '../models/types';
import {BRUSH_BASE_URL} from '../models/types';
import {traceExportRow, rowsToTsv, rowsToJson} from '../utils/export';
import type {ExportRow} from '../utils/export';
import {TraceCard} from './trace_card';
import {CrossCompareModal} from './cross_compare_modal';

// -- Module-level state --

let openFilterDropdown: string | null = null;
const PAGE_SIZE = 100;
const renderLimit = new Map<string, number>();

const ALL_FILTERS: ReadonlyArray<{id: OverviewFilter; label: string}> = [
  {id: 'all', label: 'All'},
  {id: 'positive', label: 'Positive'},
  {id: 'negative', label: 'Negative'},
  {id: 'pending', label: 'Pending'},
  {id: 'discarded', label: 'Discarded'},
];

// -- Export helpers --

function downloadFile(content: string, filename: string, mime: string): void {
  const blob = new Blob([content], {type: mime});
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 60_000);
}

function buildRows(clusters: Cluster[]): ExportRow[] {
  const rows: ExportRow[] = [];
  for (const cl of clusters) {
    for (const ts of cl.traces) {
      rows.push(traceExportRow(ts.trace, ts._key, cl.name, cl.verdicts));
    }
  }
  return rows;
}

function doExport(scope: 'tab' | 'all', format: 'json' | 'tsv'): void {
  const cl = activeCluster();
  if (!cl) return;
  const clusters = scope === 'tab' ? [cl] : S.clusters;
  const rows = buildRows(clusters);
  const date = new Date().toISOString().slice(0, 10);
  const stem = scope === 'tab' ? `qs-${cl.name}` : 'qs-all';
  const safeStem = stem.replace(/[^a-zA-Z0-9_-]/g, '_');
  if (format === 'json') {
    downloadFile(
      rowsToJson(rows),
      `${safeStem}-${date}.json`,
      'application/json',
    );
  } else {
    downloadFile(
      rowsToTsv(rows),
      `${safeStem}-${date}.tsv`,
      'text/tab-separated-values',
    );
  }
}

function doCopy(scope: 'tab' | 'all'): void {
  const cl = activeCluster();
  if (!cl) return;
  const clusters = scope === 'tab' ? [cl] : S.clusters;
  const rows = buildRows(clusters);
  navigator.clipboard.writeText(rowsToTsv(rows));
}

// -- Filter helpers --

function filterCount(cl: Cluster, filter: OverviewFilter): number {
  switch (filter) {
    case 'positive':
      return cl.counts.positive;
    case 'negative':
      return cl.counts.negative;
    case 'pending':
      return cl.counts.pending;
    case 'discarded':
      return cl.counts.discarded;
    default:
      return cl.traces.length;
  }
}

function filterIndex(filter: OverviewFilter): number {
  return ALL_FILTERS.findIndex((f) => f.id === filter);
}

// -- Sub-renders --

function renderFilterBar(
  cl: Cluster,
  activeFilter: OverviewFilter,
  onSelect: (f: OverviewFilter) => void,
): m.Children {
  return m(SegmentedButtons, {
    className: 'qs-filter-bar',
    options: ALL_FILTERS.map((f) => ({
      label: `${f.label} (${filterCount(cl, f.id)})`,
    })),
    selectedOption: filterIndex(activeFilter),
    onOptionSelected: (idx: number) => onSelect(ALL_FILTERS[idx].id),
  });
}

function renderGlobalSlider(cl: Cluster): m.Children {
  return m('.qs-trace-slider.qs-global-slider', [
    m('span.qs-slider-label', 'All'),
    m('span.qs-slider-num', String(cl.globalSlider) + '%'),
    m('input[type=range].qs-slider-input', {
      min: 1,
      max: 100,
      value: cl.globalSlider,
      step: 1,
      oninput: (e: Event) => {
        updateGlobalSlider(cl, +(e.target as HTMLInputElement).value);
      },
    }),
  ]);
}

function renderSortBtn(cl: Cluster): m.Children {
  const active = cl.sortField === 'startup_dur';
  return m(Button, {
    label: active
      ? `Startup ${cl.sortDir === 1 ? '\u2191' : '\u2193'}`
      : 'Sort',
    variant: ButtonVariant.Outlined,
    active,
    compact: true,
    tooltip: active
      ? 'Click to reverse, double-click index to reset'
      : 'Sort by startup duration',
    onclick: () => {
      if (cl.sortField === 'startup_dur') {
        cl.sortDir = cl.sortDir === 1 ? -1 : 1;
      } else {
        cl.sortField = 'startup_dur';
        cl.sortDir = 1;
      }
    },
  });
}

function renderFilterDropdown(cl: Cluster): m.Children {
  const fields = getFilterableFields(cl);
  if (fields.length === 0) return null;
  const hasActive = cl.propFilters.size > 0;

  return m('.qs-filter-dropdown-wrap', [
    m(Button, {
      label: hasActive ? `Filter (${cl.propFilters.size})` : 'Filter',
      variant: ButtonVariant.Outlined,
      active: hasActive,
      compact: true,
      onclick: (e: PointerEvent) => {
        e.stopPropagation();
        openFilterDropdown = openFilterDropdown ? null : cl.id;
      },
    }),
    openFilterDropdown === cl.id
      ? m(
          '.qs-filter-dropdown',
          {onclick: (e: Event) => e.stopPropagation()},
          fields.map((field) => {
            const values = getFieldValues(cl, field);
            const active = cl.propFilters.get(field);
            return m('.qs-filter-field', [
              m('.qs-filter-field-header', [
                m('span.qs-filter-field-name', field.replace(/_/g, ' ')),
                active
                  ? m(Button, {
                      label: 'clear',
                      variant: ButtonVariant.Minimal,
                      compact: true,
                      onclick: () => clearPropFilter(cl, field),
                    })
                  : null,
              ]),
              m(
                '.qs-filter-field-values',
                values.map((val) =>
                  m('label.qs-filter-value-label', [
                    m('input[type=checkbox]', {
                      checked: !active || active.has(val),
                      onchange: () => togglePropFilter(cl, field, val),
                    }),
                    m('span', val || '(empty)'),
                  ]),
                ),
              ),
            ]);
          }),
        )
      : null,
  ]);
}

function renderExportDropdown(): m.Children {
  const hasMultipleTabs = S.clusters.length > 1;

  const menuContent: m.Children = [
    m(MenuItem, {
      label: 'Copy (this tab)',
      icon: 'content_copy',
      onclick: () => doCopy('tab'),
    }),
    m(MenuItem, {
      label: 'JSON (this tab)',
      icon: 'download',
      onclick: () => doExport('tab', 'json'),
    }),
    m(MenuItem, {
      label: 'TSV (this tab)',
      icon: 'download',
      onclick: () => doExport('tab', 'tsv'),
    }),
  ];

  if (hasMultipleTabs) {
    menuContent.push(
      m(MenuDivider),
      m(MenuItem, {
        label: 'Copy (all tabs)',
        icon: 'content_copy',
        onclick: () => doCopy('all'),
      }),
      m(MenuItem, {
        label: 'JSON (all tabs)',
        icon: 'download',
        onclick: () => doExport('all', 'json'),
      }),
      m(MenuItem, {
        label: 'TSV (all tabs)',
        icon: 'download',
        onclick: () => doExport('all', 'tsv'),
      }),
    );
  }

  return m(
    Popup,
    {
      trigger: m(Button, {
        label: 'Export',
        icon: 'upload',
        variant: ButtonVariant.Filled,
        compact: true,
      }),
      position: PopupPosition.BottomEnd,
    },
    m(Menu, menuContent),
  );
}

function renderCardList(cl: Cluster, traces: TraceState[]): m.Children {
  // Build index map once instead of O(n) indexOf per card
  const idxMap = new Map<TraceState, number>();
  cl.traces.forEach((ts, i) => idxMap.set(ts, i));

  const limit = renderLimit.get(cl.id) ?? PAGE_SIZE;
  const visible = traces.slice(0, limit);
  const remaining = traces.length - visible.length;

  const cards = visible.map((ts) =>
    m(TraceCard, {key: ts._key, cl, ts, idx: idxMap.get(ts) ?? 0}),
  );
  const showMore =
    remaining > 0
      ? m(
          '.qs-show-more-wrap',
          m(Button, {
            label: `Show ${Math.min(remaining, PAGE_SIZE)} more (${remaining} remaining)`,
            variant: ButtonVariant.Outlined,
            className: 'qs-show-more',
            onclick: () => {
              renderLimit.set(cl.id, limit + PAGE_SIZE);
            },
          }),
        )
      : null;
  return m('.qs-trace-list', [m('.qs-trace-cards', cards), showMore]);
}

function renderOpenInBrush(
  cl: Cluster,
  filtered: TraceState[] | null,
): m.Children {
  return m(Button, {
    label: 'Open in Brush',
    variant: ButtonVariant.Outlined,
    compact: true,
    disabled: cl.traces.length === 0,
    tooltip: 'Open visible traces in Brush',
    onclick: () => {
      const visible = cl.splitView
        ? [
            ...filterTraces(cl, cl.splitFilters[0]),
            ...filterTraces(cl, cl.splitFilters[1]),
          ]
        : filtered || [];
      const uuids = visible.map((ts) => ts.trace.trace_uuid).filter(Boolean);
      if (uuids.length === 0) return;
      const filters = [
        {column: 'trace_uuid', operator: 'in', value: JSON.stringify(uuids)},
      ];
      const encoded = encodeURIComponent(JSON.stringify(filters));
      const url =
        `${BRUSH_BASE_URL}?filters=${encoded}` +
        '&metric_id=android_startup&charts=gallery' +
        '&gallerySvgColumn=svg&galleryMetricColumn=dur_ms' +
        '&galleryMetricNameColumn=process_name';
      window.open(url, '_blank');
    },
  });
}

// -- Toolbar --

function renderToolbar(cl: Cluster, filtered: TraceState[] | null): m.Children {
  return m('.qs-list-toolbar', [
    cl.splitView
      ? m('.qs-list-filters-label', 'Split View')
      : renderFilterBar(cl, cl.overviewFilter, (f) => {
          cl.overviewFilter = f;
        }),
    renderGlobalSlider(cl),
    m('.qs-list-actions', [
      renderSortBtn(cl),
      renderFilterDropdown(cl),
      m(Button, {
        label: cl.splitView ? 'Single' : 'Split',
        variant: ButtonVariant.Outlined,
        active: cl.splitView,
        compact: true,
        tooltip: 'Toggle split view',
        onclick: () => {
          cl.splitView = !cl.splitView;
        },
      }),
      m(Button, {
        label: 'Copy to tab',
        variant: ButtonVariant.Outlined,
        compact: true,
        disabled: cl.splitView || !filtered || filtered.length === 0,
        tooltip: cl.splitView
          ? 'Switch to single view first'
          : 'Copy visible traces to a new tab',
        onclick: () => {
          if (filtered) copyFilteredToNewTab(cl, filtered);
        },
      }),
      m(Button, {
        label: 'Compare',
        variant: ButtonVariant.Outlined,
        compact: true,
        disabled: cl.traces.length < 2,
        tooltip: 'Compare traces in pairs to find groups',
        onclick: () => startCrossCompare(cl),
      }),
      renderOpenInBrush(cl, filtered),
      renderExportDropdown(),
    ]),
  ]);
}

// -- Split view --

function renderSplitView(cl: Cluster): m.Children {
  const leftTraces = filterTraces(cl, cl.splitFilters[0]);
  const rightTraces = filterTraces(cl, cl.splitFilters[1]);

  const leftPanel = m('.qs-split-panel-inner', [
    m('.qs-split-panel-header', [
      renderFilterBar(cl, cl.splitFilters[0], (f) => {
        cl.splitFilters[0] = f;
      }),
      m('span.qs-split-count', `${leftTraces.length}`),
    ]),
    m('.qs-split-panel-body', renderCardList(cl, leftTraces)),
  ]);

  const rightPanel = m('.qs-split-panel-inner', [
    m('.qs-split-panel-header', [
      renderFilterBar(cl, cl.splitFilters[1], (f) => {
        cl.splitFilters[1] = f;
      }),
      m('span.qs-split-count', `${rightTraces.length}`),
    ]),
    m('.qs-split-panel-body', renderCardList(cl, rightTraces)),
  ]);

  return m(SplitPanel, {
    className: 'qs-split-container',
    direction: 'horizontal',
    initialSplit: {percent: cl.splitRatio * 100},
    firstPanel: leftPanel,
    secondPanel: rightPanel,
    onResize: (pct: number) => {
      cl.splitRatio = pct / 100;
    },
  });
}

// -- Document click handler for closing dropdowns --

let docClickHandler: (() => void) | null = null;

// -- Main component --

export class TraceList implements m.ClassComponent {
  oncreate(): void {
    docClickHandler = () => {
      if (openFilterDropdown) {
        openFilterDropdown = null;
        m.redraw();
      }
    };
    document.addEventListener('click', docClickHandler);
  }

  onremove(): void {
    if (docClickHandler) {
      document.removeEventListener('click', docClickHandler);
      docClickHandler = null;
    }
  }

  view(): m.Children {
    const cl = activeCluster();
    if (!cl || cl.traces.length === 0) {
      return m('.qs-section', [
        m('.qs-section-head', 'Traces'),
        m(
          'p',
          {style: {color: 'var(--dim)', fontSize: '11px'}},
          'No traces loaded.',
        ),
      ]);
    }

    const filtered = cl.splitView ? null : filteredTraces();
    const toolbar = renderToolbar(cl, filtered);
    const ccModal = getCrossCompareState() ? m(CrossCompareModal, {cl}) : null;

    if (filtered) {
      // Normal single-panel view
      const countLabel =
        filtered.length !== cl.traces.length
          ? `${filtered.length}/${cl.traces.length}`
          : `${filtered.length}`;
      return [
        m('.qs-section', [
          m('.qs-section-head', `Traces (${countLabel})`),
          toolbar,
          renderCardList(cl, filtered),
        ]),
        ccModal,
      ];
    }

    // Split view
    return [
      m('.qs-section', [
        m('.qs-section-head', 'Traces (split view)'),
        toolbar,
        renderSplitView(cl),
      ]),
      ccModal,
    ];
  }
}
