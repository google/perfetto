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
import {
  ClassNameWithWarning,
  MERGED_CLASS_WARNING_DESCRIPTION,
  MERGED_CLASS_WARNING_ICON,
  MERGED_CLASS_WARNING_TITLE,
  type MergedClassWarningTone,
} from '../../../components/merged_class_warning';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import type {ColumnSchema} from '../../../components/widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from '../../../components/widgets/datagrid/in_memory_data_source';
import type {Row, SqlValue} from '../../../trace_processor/query_result';
import {Anchor} from '../../../widgets/anchor';
import {Flamegraph} from '../../../widgets/flamegraph';
import {PopupPosition} from '../../../widgets/popup';
import {RadioGroup} from '../../../widgets/radio_group';
import type {
  TreeExplorerData,
  TreeExplorerMarker,
  TreeExplorerMetric,
  TreeExplorerNode,
  TreeExplorerState,
} from '../../../widgets/tree_explorer';
import {
  EnumOption,
  renderDocSection,
  renderWidgetShowcase,
} from '../widgets_page_utils';

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

function fmtBytes(value: SqlValue): string {
  const n = Number(value ?? 0);
  if (n >= 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} MiB`;
  if (n >= 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${n} B`;
}

function sizeCell(value: SqlValue) {
  return {content: m('span', fmtBytes(value)), align: 'right' as const};
}

function shortName(cls: string): string {
  const dot = cls.lastIndexOf('.');
  return dot >= 0 ? cls.slice(dot + 1) : cls;
}

// Classes flagged as potentially merged. In the real UI this would be loaded
// once per heap dump from heap_graph_class and looked up by name, since
// DataGrid cell renderers only receive the visible columns of a row.
const MERGED_CLASSES: ReadonlySet<string> = new Set([
  'com.example.app.image.ImageCache',
  'com.example.app.ui.FeedAdapter',
  'com.example.app.model.Post',
  'com.example.app.di.AppComponent',
]);

function isMerged(cls: string): boolean {
  return MERGED_CLASSES.has(cls);
}

// Icon color, switched page-wide from the control at the top of the page.
let pageTone: MergedClassWarningTone = 'warning';

// ---------------------------------------------------------------------------
// Tables (mimic the Heap Dump Explorer Classes and Objects tabs)
// ---------------------------------------------------------------------------

const CLASS_ROWS: Row[] = [
  {
    cls: 'android.graphics.Bitmap',
    cnt: 42,
    shallow: 2048,
    retained: 8_400_000,
  },
  {
    cls: 'com.example.app.image.ImageCache',
    cnt: 1,
    shallow: 64,
    retained: 4_900_000,
  },
  {
    cls: 'com.example.app.ui.FeedAdapter',
    cnt: 3,
    shallow: 192,
    retained: 2_300_000,
  },
  {
    cls: 'java.lang.String',
    cnt: 18_211,
    shallow: 437_064,
    retained: 1_200_000,
  },
  {
    cls: 'com.example.app.model.Post',
    cnt: 640,
    shallow: 30_720,
    retained: 610_000,
  },
  {
    cls: 'java.util.HashMap',
    cnt: 1_532,
    shallow: 73_536,
    retained: 540_000,
  },
  {
    cls: 'com.example.app.di.AppComponent',
    cnt: 1,
    shallow: 48,
    retained: 120_000,
  },
  {
    cls: 'android.content.res.Resources',
    cnt: 2,
    shallow: 112,
    retained: 96_000,
  },
];

const OBJECT_ROWS: Row[] = [
  {
    id: 0x12c4a0e8,
    cls: 'com.example.app.image.ImageCache',
    shallow: 64,
    retained: 4_900_000,
  },
  {
    id: 0x12c51f20,
    cls: 'android.graphics.Bitmap',
    shallow: 48,
    retained: 3_686_400,
  },
  {
    id: 0x12c7d300,
    cls: 'com.example.app.ui.FeedAdapter',
    shallow: 64,
    retained: 1_900_000,
  },
  {
    id: 0x12c7d348,
    cls: 'com.example.app.ui.FeedAdapter',
    shallow: 64,
    retained: 300_000,
  },
  {
    id: 0x12d01a10,
    cls: 'java.lang.String',
    shallow: 24,
    retained: 4_120,
  },
  {
    id: 0x12d20b88,
    cls: 'com.example.app.model.Post',
    shallow: 48,
    retained: 2_400,
  },
  {
    id: 0x12d20bb8,
    cls: 'com.example.app.model.Post',
    shallow: 48,
    retained: 2_150,
  },
  {
    id: 0x12e00c40,
    cls: 'java.util.HashMap',
    shallow: 48,
    retained: 1_024,
  },
];

const classesDataSource = new InMemoryDataSource(CLASS_ROWS);
const objectsDataSource = new InMemoryDataSource(OBJECT_ROWS);

// Object id -> class name, so the object label can be built without relying
// on the Class column being visible.
const OBJECT_CLASS = new Map(
  OBJECT_ROWS.map((r) => [Number(r.id), String(r.cls)]),
);

function classCell(value: SqlValue): m.Children {
  const cls = String(value);
  return m(ClassNameWithWarning, {
    name: m(Anchor, {}, cls),
    potentiallyMerged: isMerged(cls),
    tone: pageTone,
  });
}

const CLASSES_SCHEMA: ColumnSchema = {
  cls: {title: 'Class', columnType: 'text', cellRenderer: classCell},
  cnt: {title: 'Count', columnType: 'quantitative'},
  shallow: {
    title: 'Shallow',
    columnType: 'quantitative',
    cellRenderer: sizeCell,
  },
  retained: {
    title: 'Retained',
    columnType: 'quantitative',
    cellRenderer: sizeCell,
  },
};

function objectsSchema(iconInObjectColumn: boolean): ColumnSchema {
  return {
    id: {
      title: 'Object',
      columnType: 'text',
      cellRenderer: (value: SqlValue) => {
        const id = Number(value);
        const cls = OBJECT_CLASS.get(id) ?? '';
        const label = `${shortName(cls)} 0x${id.toString(16)}`;
        return m(ClassNameWithWarning, {
          name: m(Anchor, {}, label),
          potentiallyMerged: iconInObjectColumn && isMerged(cls),
          tone: pageTone,
        });
      },
    },
    cls: {title: 'Class', columnType: 'text', cellRenderer: classCell},
    shallow: {
      title: 'Shallow',
      columnType: 'quantitative',
      cellRenderer: sizeCell,
    },
    retained: {
      title: 'Retained',
      columnType: 'quantitative',
      cellRenderer: sizeCell,
    },
  };
}

// ---------------------------------------------------------------------------
// Flamegraph (mimics the Heap Dump Explorer class-tree flamegraph)
// ---------------------------------------------------------------------------

interface ClassTreeSpec {
  readonly name: string;
  readonly self: number;
  readonly children?: ReadonlyArray<ClassTreeSpec>;
}

const CLASS_TREE: ReadonlyArray<ClassTreeSpec> = [
  {
    name: 'com.example.app.MainActivity',
    self: 2_000,
    children: [
      {
        name: 'com.example.app.ui.FeedAdapter',
        self: 40_000,
        children: [
          {
            name: 'com.example.app.ui.PostViewHolder',
            self: 120_000,
            children: [{name: 'android.graphics.Bitmap', self: 800_000}],
          },
          {
            name: 'java.util.ArrayList',
            self: 8_000,
            children: [{name: 'com.example.app.model.Post', self: 300_000}],
          },
        ],
      },
      {
        name: 'com.example.app.image.ImageCache',
        self: 16_000,
        children: [
          {
            name: 'android.util.LruCache',
            self: 4_000,
            children: [{name: 'android.graphics.Bitmap', self: 1_200_000}],
          },
        ],
      },
    ],
  },
  {
    name: 'android.app.ActivityThread',
    self: 4_000,
    children: [
      {
        name: 'android.content.res.Resources',
        self: 30_000,
        children: [{name: 'java.lang.String', self: 200_000}],
      },
    ],
  },
  {
    name: 'com.example.app.di.AppComponent',
    self: 10_000,
    children: [{name: 'java.util.HashMap', self: 20_000}],
  },
];

type MarkerStyle = 'icon' | 'square (existing)';

const METRIC_NAME = 'Object Size';
const FLAMEGRAPH_METRICS: ReadonlyArray<TreeExplorerMetric> = [
  {name: METRIC_NAME, unit: 'B', nameColumnLabel: 'Class'},
];

function markerFor(
  style: MarkerStyle,
  tone: MergedClassWarningTone,
): TreeExplorerMarker {
  if (style === 'icon') {
    return {
      name: MERGED_CLASS_WARNING_TITLE,
      icon: MERGED_CLASS_WARNING_ICON,
      description: MERGED_CLASS_WARNING_DESCRIPTION,
      tone,
    };
  }
  return {name: MERGED_CLASS_WARNING_TITLE};
}

function cumulative(spec: ClassTreeSpec): number {
  return (spec.children ?? []).reduce((s, c) => s + cumulative(c), spec.self);
}

function buildFlamegraphData(
  style: MarkerStyle,
  tone: MergedClassWarningTone,
): TreeExplorerData {
  const nodes: TreeExplorerNode[] = [];
  const marker = markerFor(style, tone);
  let nextId = 0;
  let maxDepth = 0;

  // Pre-order traversal, children laid out left to right within the parent.
  const visit = (
    spec: ClassTreeSpec,
    parentId: number,
    depth: number,
    xStart: number,
    parentCumulative: number | undefined,
  ) => {
    const id = nextId++;
    const cum = cumulative(spec);
    maxDepth = Math.max(maxDepth, depth);
    nodes.push({
      id,
      parentId,
      depth,
      name: spec.name,
      selfValue: spec.self,
      cumulativeValue: cum,
      parentCumulativeValue: parentCumulative,
      properties: new Map(),
      marker: isMerged(spec.name) ? marker : undefined,
      xStart,
      xEnd: xStart + cum,
    });
    let childX = xStart;
    for (const child of spec.children ?? []) {
      visit(child, id, depth + 1, childX, cum);
      childX += cumulative(child);
    }
  };

  let x = 0;
  for (const root of CLASS_TREE) {
    visit(root, -1, 1, x, undefined);
    x += cumulative(root);
  }

  return {
    nodes,
    unfilteredCumulativeValue: x,
    allRootsCumulativeValue: x,
    minDepth: 0,
    maxDepth,
    nodeActions: [],
    rootActions: [],
  };
}

// Cached so the Flamegraph does not reset its zoom/hover on every redraw.
const flamegraphDataCache = new Map<string, TreeExplorerData>();
function flamegraphData(
  style: MarkerStyle,
  tone: MergedClassWarningTone,
): TreeExplorerData {
  const key = `${style}:${tone}`;
  let data = flamegraphDataCache.get(key);
  if (data === undefined) {
    data = buildFlamegraphData(style, tone);
    flamegraphDataCache.set(key, data);
  }
  return data;
}

let flamegraphState: TreeExplorerState = {
  selectedMetricId: METRIC_NAME,
  addedMetricIds: [],
  displayMode: 'flamegraph',
  filters: [],
  view: {kind: 'TOP_DOWN'},
};

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

export function renderMergedClassWarning(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'MergedClassWarning'),
      m(
        'p',
        'Warning shown next to Java class names in heap dumps when the class ',
        'may have been produced by merging several source classes during app ',
        'optimization (e.g. R8 class merging). Hover the icon for details. ',
        'All data on this page is static mock data.',
      ),
      m(
        'p',
        'Icon color (applies to every section): ',
        m(
          RadioGroup,
          {
            selectedValue: pageTone,
            onValueChange: (v: string) => {
              pageTone = v as MergedClassWarningTone;
            },
          },
          [
            m(RadioGroup.Button, {value: 'warning'}, 'Amber'),
            m(RadioGroup.Button, {value: 'muted'}, 'Gray'),
          ],
        ),
      ),
    ),

    renderDocSection('Standalone', [
      m(
        'p',
        'ClassNameWithWarning renders the class name followed by the warning ',
        'icon when the class is flagged.',
      ),
    ]),
    renderWidgetShowcase({
      renderWidget: ({potentiallyMerged, position}) =>
        m(ClassNameWithWarning, {
          name: 'com.example.app.image.ImageCache',
          potentiallyMerged,
          position: position as PopupPosition,
          tone: pageTone,
        }),
      initialOpts: {
        potentiallyMerged: true,
        position: new EnumOption(
          PopupPosition.Top,
          Object.values(PopupPosition),
        ),
      },
    }),

    renderDocSection('In a table: Classes tab', [
      m('p', 'The icon follows the class name in the Class column.'),
    ]),
    m(
      '',
      {style: {height: '320px'}},
      m(DataGrid, {
        schema: CLASSES_SCHEMA,
        data: classesDataSource,
        fillHeight: true,
        initialColumns: [
          {id: 'cls', field: 'cls'},
          {id: 'cnt', field: 'cnt'},
          {id: 'shallow', field: 'shallow'},
          {id: 'retained', field: 'retained', sort: 'DESC' as const},
        ],
      }),
    ),

    renderDocSection('In a table: Objects tab', [
      m(
        'p',
        'The icon always follows the Class column. Toggle ',
        m('code', 'iconInObjectColumn'),
        ' to also show it in the object label (repeats once per object).',
      ),
    ]),
    renderWidgetShowcase({
      renderWidget: ({iconInObjectColumn}) =>
        m(
          '',
          {style: {height: '320px', width: '100%'}},
          m(DataGrid, {
            schema: objectsSchema(iconInObjectColumn),
            data: objectsDataSource,
            fillHeight: true,
            initialColumns: [
              {id: 'id', field: 'id'},
              {id: 'cls', field: 'cls'},
              {id: 'shallow', field: 'shallow'},
              {id: 'retained', field: 'retained', sort: 'DESC' as const},
            ],
          }),
        ),
      initialOpts: {
        iconInObjectColumn: true,
      },
      noPadding: true,
    }),

    renderDocSection('In the flamegraph', [
      m(
        'p',
        'Flagged nodes draw the warning glyph before the class name on the ',
        'canvas. Hovering a node shows the warning as a callout at the top of ',
        'the existing node tooltip, so there is only ever one tooltip. Switch ',
        m('code', 'markerStyle'),
        ' to compare with the existing small-square marker.',
      ),
    ]),
    renderWidgetShowcase({
      renderWidget: ({markerStyle}) =>
        m(
          '',
          {style: {height: '220px', width: '100%'}},
          m(Flamegraph, {
            metrics: FLAMEGRAPH_METRICS,
            state: flamegraphState,
            data: flamegraphData(markerStyle as MarkerStyle, pageTone),
            onStateChange: (s) => {
              flamegraphState = s;
            },
          }),
        ),
      initialOpts: {
        markerStyle: new EnumOption<MarkerStyle[]>('icon', [
          'icon',
          'square (existing)',
        ]),
      },
      noPadding: true,
    }),
  ];
}
