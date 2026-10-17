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
import {sqliteString} from '../../../base/string_utils';
import {Memo} from '../../../base/memo';
import type {Trace} from '../../../public/trace';
import type {SqlValue} from '../../../trace_processor/query_result';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import type {
  CellRenderResult,
  ColumnSchema,
} from '../../../components/widgets/datagrid/datagrid_schema';
import type {Filter} from '../../../components/widgets/datagrid/model';
import {Anchor} from '../../../widgets/anchor';
import {Button} from '../../../widgets/button';
import {DetailsShell} from '../../../widgets/details_shell';
import {ClassNameWithWarning} from '../../../components/merged_class_warning';
import {fmtHex} from '../format';
import {
  type NavFn,
  sizeRenderer,
  countRenderer,
  shortClassName,
  shortFieldName,
  NO_FIELD_LABEL,
  RowCounter,
  rowCountHeading,
  colHeader,
  classNameCellRenderer,
  COL_INFO,
} from '../components';
import type {CollectionsParams} from '../nav_state';
import type {HeapDump} from '../types';

export interface CollectionsViewAttrs {
  readonly trace: Trace;
  readonly activeDump: HeapDump;
  readonly navigate: NavFn;
  readonly potentiallyMergedClasses: ReadonlySet<string>;
  readonly params: CollectionsParams;
  readonly onParamsChange: (params: CollectionsParams) => void;
  // The flamegraph frame(s) drilled down into. `collectionCls` is set when
  // they are collection frames: then only those collections are listed, not
  // the collections nested in them.
  readonly scope: {
    readonly pathHashes: string;
    readonly label: string;
    readonly collectionCls?: string;
  };
}

const TREE_MODULE = 'android.memory.heap_graph.collection_tree';

const INFO = {
  instances:
    'Collections with this owning field and class. Collections nested in ' +
    "another one are rows of their own (e.g. 'Foo.mMap[value]'), so the " +
    'totals can exceed those of the flamegraph, which counts the outermost ' +
    'collections only.',
  elements: 'Elements held, over all instances. Entries for maps.',
  retained:
    'Memory owned by the collections: what freeing them releases. ' +
    'Collections inside another one of the same row count once.',
  reachable:
    'Memory reachable from the collections (shortest path tree). ' +
    'Can include objects shared with other owners.',
  unsupported:
    'No recipe for this class: sizes are exact, elements are not counted.',
} as const;

const DASH = '—';

const quantCol = (
  title: string,
  info: string,
  renderer: (v: SqlValue) => CellRenderResult,
): ColumnSchema[string] => ({
  title: colHeader(title, info),
  titleString: title,
  columnType: 'quantitative',
  cellRenderer: (v) =>
    v === null
      ? {content: m('span.pf-hde-mono', DASH), align: 'right'}
      : renderer(v),
});

const textCol = (
  title: string,
  cellRenderer?: (v: SqlValue, r: Record<string, SqlValue>) => m.Children,
): ColumnSchema[string] => ({
  title,
  columnType: 'text',
  ...(cellRenderer && {cellRenderer}),
});

const classCell = (v: SqlValue) =>
  v === null ? DASH : m('span', {title: String(v)}, shortClassName(String(v)));

// 'ArrayList 0x1a2b', or just the id when the class is unknown.
const objectLabel = (id: number, cls: string | undefined) =>
  cls ? `${shortClassName(cls)} ${fmtHex(id)}` : fmtHex(id);

function objectLink(
  id: number,
  cls: string | undefined,
  navigate: NavFn,
): m.Children {
  const label = objectLabel(id, cls);
  return m(Anchor, {onclick: () => navigate('object', {id, label})}, label);
}

const unsupportedBadge = (layout: SqlValue): m.Children =>
  layout === 'unsupported' &&
  m('span.pf-hde-badge-unsupported', {title: INFO.unsupported}, 'unsupported');

// The collections of the active dump in or below the flamegraph frames with
// `pathHashes` (CSV).
// The collections under the frames `pathHashes`. With `leafOnly`, only the
// collections that are leaf frames themselves, not those nested in them.
function collectionsSql(
  dump: HeapDump,
  pathHashes: string,
  leafOnly: boolean,
): string {
  const inDump = `upid = ${dump.upid} AND graph_sample_ts = ${dump.ts}`;
  const hashes = pathHashes.split(',').filter((h) => /^-?\d+$/.test(h));
  if (hashes.length === 0) {
    return `(SELECT * FROM _heap_graph_collection WHERE FALSE)`;
  }
  const frames = `SELECT column1 AS path_hash FROM (VALUES ${hashes
    .map((h) => `(${h})`)
    .join(', ')})`;
  const leaf = leafOnly ? ' WHERE is_leaf' : '';
  return `(SELECT * FROM _heap_graph_collection WHERE ${inDump}
    AND id IN (SELECT id FROM _heap_graph_collections_under_frames!((${frames}))${leaf}))`;
}

function fieldFilterSql({field, noField, cls}: CollectionsParams) {
  const f = noField
    ? 'c.field_name IS NULL'
    : field === undefined
      ? 'TRUE'
      : `c.field_name = ${sqliteString(field)}`;
  return cls === undefined ? f : `${f} AND c.class_name = ${sqliteString(cls)}`;
}

const summarySql = (collections: string) => `
  SELECT field_name, class_name, element_class, kind, layout, instance_count,
    element_count_sum AS element_count, retained_size_sum AS retained_size,
    reachable_size_sum AS reachable_size
  FROM _heap_graph_collection_summary!(${collections})`;

const instancesSql = (collections: string, filter: string) => `
  SELECT c.id, c.owner_id, coalesce(oc.deobfuscated_name, oc.name)
    AS owner_class, c.element_class, c.element_count, c.retained_size,
    c.reachable_size
  FROM ${collections} AS c
  LEFT JOIN heap_graph_object AS oo ON oo.id = c.owner_id
  LEFT JOIN heap_graph_class AS oc ON oc.id = oo.type_id
  WHERE ${filter}`;

const elementsSql = (collectionId: number) => `
  SELECT e.slot_index, e.part, e.element_id,
    ifnull(c.deobfuscated_name, c.name) AS cls, o.self_size
  FROM _heap_graph_collection_element AS e
  JOIN heap_graph_object AS o ON o.id = e.element_id
  JOIN heap_graph_class AS c ON o.type_id = c.id
  WHERE e.collection_id = ${collectionId}`;

const sizeCols = {
  retained_size: quantCol('Retained', INFO.retained, sizeRenderer),
  reachable_size: quantCol('Reachable', INFO.reachable, sizeRenderer),
};

function summarySchema(
  onDrill: (params: CollectionsParams) => void,
  potentiallyMerged: ReadonlySet<string>,
): ColumnSchema {
  // Columns can be hidden: drill down by those shown.
  const drill = (r: Record<string, SqlValue>) => () =>
    onDrill({
      ...('field_name' in r &&
        (r.field_name === null
          ? {noField: true}
          : {field: String(r.field_name)})),
      ...('class_name' in r && {cls: String(r.class_name)}),
    });
  return {
    field_name: textCol('Field', (v, r) =>
      m(
        Anchor,
        {
          title: v === null ? 'Not held by a field' : String(v),
          onclick: drill(r),
        },
        v === null ? NO_FIELD_LABEL : shortFieldName(String(v)),
      ),
    ),
    class_name: textCol('Collection', (v, r) =>
      m('span', [
        m(ClassNameWithWarning, {
          name: m(Anchor, {title: String(v), onclick: drill(r)}, String(v)),
          potentiallyMerged: potentiallyMerged.has(String(v)),
        }),
        unsupportedBadge(r.layout),
      ]),
    ),
    element_class: textCol('Element type', classCell),
    kind: textCol('Kind'),
    layout: textCol('Layout'),
    instance_count: quantCol('Instances', INFO.instances, countRenderer),
    element_count: quantCol('Elements', INFO.elements, countRenderer),
    ...sizeCols,
  };
}

// `cls` is the class of the collections, if known.
function instancesSchema(
  cls: string | undefined,
  navigate: NavFn,
  onSelect: (id: number) => void,
): ColumnSchema {
  return {
    id: {
      title: 'Collection',
      columnType: 'identifier',
      cellRenderer: (v) => objectLink(Number(v), cls, navigate),
    },
    // The owner's class labels the link when its column is shown (rows only
    // hold the visible columns).
    owner_id: {
      title: 'Owner',
      columnType: 'identifier',
      cellRenderer: (v, r) =>
        v === null
          ? DASH
          : objectLink(
              Number(v),
              typeof r.owner_class === 'string' ? r.owner_class : undefined,
              navigate,
            ),
    },
    owner_class: textCol('Owner type', classCell),
    element_class: textCol('Element type', classCell),
    element_count: {
      title: colHeader('Elements', INFO.elements),
      titleString: 'Elements',
      columnType: 'quantitative',
      cellRenderer: (v, r) => {
        if (v === null) return {content: DASH, align: 'right'};
        if (Number(v) === 0) return countRenderer(v);
        const id = Number(r.id);
        return {
          content: m(
            Anchor,
            {
              title: `Show the elements of ${objectLabel(id, cls)}`,
              onclick: () => onSelect(id),
            },
            Number(v).toLocaleString(),
          ),
          align: 'right',
        };
      },
    },
    ...sizeCols,
  };
}

function elementsSchema(
  navigate: NavFn,
  potentiallyMerged: ReadonlySet<string>,
): ColumnSchema {
  return {
    slot_index: {
      title: 'Slot',
      columnType: 'quantitative',
      cellRenderer: (v) =>
        v === null ? {content: DASH, align: 'right'} : countRenderer(v),
    },
    part: textCol('Part', (v) => (v === null ? DASH : String(v))),
    element_id: {
      title: 'Element',
      columnType: 'identifier',
      cellRenderer: (v, r) =>
        objectLink(
          Number(v),
          r.cls === null ? undefined : String(r.cls),
          navigate,
        ),
    },
    cls: textCol('Class', classNameCellRenderer(potentiallyMerged)),
    self_size: {
      title: colHeader('Shallow', COL_INFO.shallow),
      titleString: 'Shallow',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
  };
}

const columns = (...ids: string[]) => ids.map((id) => ({id, field: id}));
const sortedBy = (id: string, dir: 'ASC' | 'DESC') => ({
  id,
  field: id,
  sort: dir,
});

const SUMMARY_COLUMNS = [
  ...columns(
    'field_name',
    'class_name',
    'element_class',
    'kind',
    'layout',
    'instance_count',
    'element_count',
  ),
  sortedBy('retained_size', 'DESC'),
  ...columns('reachable_size'),
];

const INSTANCE_COLUMNS = [
  ...columns('id', 'owner_id', 'owner_class', 'element_class', 'element_count'),
  sortedBy('retained_size', 'DESC'),
  ...columns('reachable_size'),
];

const ELEMENT_COLUMNS = [
  sortedBy('slot_index', 'ASC'),
  ...columns('part', 'element_id', 'cls', 'self_size'),
];

interface Level {
  readonly id: string;
  readonly title: string;
  readonly query: string;
  readonly schema: ColumnSchema;
  readonly columns: ReadonlyArray<{
    readonly id: string;
    readonly field: string;
    readonly sort?: 'ASC' | 'DESC';
  }>;
  readonly back?: {readonly label: string; readonly onclick: () => void};
}

export function CollectionsView(): m.Component<CollectionsViewAttrs> {
  const dataSource = new Memo<SQLDataSource>();
  const counter = new RowCounter();
  let filters: readonly Filter[] = [];
  let filteredLevel: string | undefined;

  function level(attrs: CollectionsViewAttrs): Level {
    const {navigate, potentiallyMergedClasses, params, onParamsChange, scope} =
      attrs;
    const collections = collectionsSql(
      attrs.activeDump,
      scope.pathHashes,
      scope.collectionCls !== undefined,
    );
    const cls = params.cls ?? scope.collectionCls;
    if (params.collection !== undefined) {
      const {collection, ...instances} = params;
      return {
        id: 'elements',
        title: `Elements of ${objectLabel(collection, cls)}`,
        query: elementsSql(collection),
        schema: elementsSchema(navigate, potentiallyMergedClasses),
        columns: ELEMENT_COLUMNS,
        back: {
          label: 'Back to instances',
          onclick: () => onParamsChange(instances),
        },
      };
    }
    if (scope.collectionCls !== undefined) {
      return {
        id: 'instances',
        title: scope.label,
        query: instancesSql(collections, 'TRUE'),
        schema: instancesSchema(scope.collectionCls, navigate, (collection) =>
          onParamsChange({...params, collection}),
        ),
        columns: INSTANCE_COLUMNS,
      };
    }
    if (
      params.field !== undefined ||
      params.noField ||
      params.cls !== undefined
    ) {
      const field = params.noField
        ? NO_FIELD_LABEL
        : params.field === undefined
          ? ''
          : shortFieldName(params.field);
      const clsLabel = cls === undefined ? '' : ` (${shortClassName(cls)})`;
      return {
        id: 'instances',
        title: `${field}${clsLabel}`,
        query: instancesSql(collections, fieldFilterSql(params)),
        schema: instancesSchema(cls, navigate, (collection) =>
          onParamsChange({...params, collection}),
        ),
        columns: INSTANCE_COLUMNS,
        back: {label: 'Back to fields', onclick: () => onParamsChange({})},
      };
    }
    return {
      id: 'summary',
      title: `Collections under ${scope.label}`,
      query: summarySql(collections),
      schema: summarySchema(onParamsChange, potentiallyMergedClasses),
      columns: SUMMARY_COLUMNS,
    };
  }

  return {
    onremove() {
      dataSource.dispose();
      counter.dispose();
    },
    view({attrs}) {
      const {trace} = attrs;
      const lvl = level(attrs);
      if (lvl.query !== filteredLevel) {
        filters = [];
        filteredLevel = lvl.query;
      }
      const preamble = `INCLUDE PERFETTO MODULE ${TREE_MODULE};`;
      const data = dataSource.use({
        key: {query: lvl.query},
        compute: () =>
          new SQLDataSource({
            engine: trace.engine,
            tableOrSubquery: lvl.query,
            preamble,
          }),
      });
      return m(
        DetailsShell,
        {
          title: rowCountHeading(
            lvl.title,
            counter.use({
              engine: trace.engine,
              query: lvl.query,
              preamble,
              filters,
            }),
          ),
          buttons: lvl.back && m(Button, {icon: 'arrow_back', ...lvl.back}),
          fillHeight: true,
        },
        m(DataGrid, {
          // Each level has its own columns: recreate the grid on change.
          key: lvl.id,
          schema: lvl.schema,
          data,
          fillHeight: true,
          initialColumns: lvl.columns,
          filters,
          showExportButton: true,
          onFiltersChanged: (f) => (filters = [...f]),
        }),
      );
    },
  };
}
