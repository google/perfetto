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
import type {Engine} from '../../../trace_processor/engine';
import type {SqlValue} from '../../../trace_processor/query_result';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import type {ColumnSchema} from '../../../components/widgets/datagrid/datagrid_schema';
import {fmtHex} from '../format';
import {
  type NavFn,
  sizeRenderer,
  countRenderer,
  shortClassName,
  SQL_PREAMBLE,
  RowCounter,
  COL_INFO,
  colHeader,
} from '../components';
import {Anchor} from '../../../widgets/anchor';
import {DetailsShell} from '../../../widgets/details_shell';
import {Memo} from '../../../base/memo';

interface FlamegraphObjectsViewAttrs {
  readonly engine: Engine;
  readonly navigate: NavFn;
  readonly pathHashes?: string;
  readonly isDominator?: boolean;
  readonly onBackToTimeline?: () => void;
  readonly nodeName?: string;
}

export function flamegraphQuery(
  pathHashes: string,
  isDominator: boolean,
): string {
  const hashTable = isDominator
    ? '_heap_graph_dominator_path_hashes'
    : '_heap_graph_path_hashes';
  const values = pathHashes
    .split(',')
    .map((v) => `(${v.trim()})`)
    .join(', ');
  return `
    WITH _hde_sel(path_hash) AS (VALUES ${values})
    SELECT
      o.id,
      ifnull(c.deobfuscated_name, c.name) AS cls,
      o.self_size,
      o.native_size,
      ifnull(d.dominated_size_bytes, o.self_size) AS retained,
      ifnull(d.dominated_native_size_bytes, o.native_size) AS retained_native,
      ifnull(d.dominated_obj_count, 1) AS retained_count,
      a.cumulative_size AS reachable_size,
      a.cumulative_native_size AS reachable_native,
      CAST(a.cumulative_count AS INT) AS reachable_count,
      ifnull(o.heap_type, 'default') AS heap,
      od.value_string AS str
    FROM _hde_sel f
    JOIN ${hashTable} h ON h.path_hash = f.path_hash
    JOIN heap_graph_object o ON o.id = h.id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    LEFT JOIN _heap_graph_object_tree_aggregation a ON a.id = o.id
  `;
}

function makeUiSchema(navigate: NavFn): ColumnSchema {
  return {
    id: {
      title: 'Object',
      columnType: 'identifier',
      cellRenderer: (value: SqlValue, row) => {
        const id = Number(value);
        const cls = String(row.cls ?? '');
        const display = `${shortClassName(cls)} ${fmtHex(id)}`;
        const str = row.str != null ? String(row.str) : null;
        return m('span', [
          m(
            Anchor,
            {
              onclick: () =>
                navigate('object', {id, label: str ? `"${str}"` : display}),
            },
            display,
          ),
          str
            ? m(
                'span',
                {class: 'pf-hde-str-badge'},
                ` "${str.length > 40 ? str.slice(0, 40) + '\u2026' : str}"`,
              )
            : null,
        ]);
      },
    },
    self_size: {
      title: colHeader('Shallow', COL_INFO.shallow),
      titleString: 'Shallow',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    native_size: {
      title: colHeader('Native', COL_INFO.shallowNative),
      titleString: 'Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained: {
      title: colHeader('Retained', COL_INFO.retained),
      titleString: 'Retained',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained_native: {
      title: colHeader('Retained Native', COL_INFO.retainedNative),
      titleString: 'Retained Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained_count: {
      title: colHeader('Retained #', COL_INFO.retainedCount),
      titleString: 'Retained #',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
    reachable_size: {
      title: colHeader('Reachable', COL_INFO.reachable),
      titleString: 'Reachable',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_native: {
      title: colHeader('Reachable Native', COL_INFO.reachableNative),
      titleString: 'Reachable Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_count: {
      title: colHeader('Reachable #', COL_INFO.reachableCount),
      titleString: 'Reachable #',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
    heap: {
      title: 'Heap',
      columnType: 'text',
    },
    cls: {
      title: 'Class',
      columnType: 'text',
    },
    str: {
      title: 'String Value',
      columnType: 'text',
    },
  };
}

export function FlamegraphObjectsView(): m.Component<FlamegraphObjectsViewAttrs> {
  const datasourceMemo = new Memo<SQLDataSource | null>();
  const counter = new RowCounter();

  return {
    onremove() {
      datasourceMemo.dispose();
    },
    view(vnode) {
      const {
        engine,
        pathHashes,
        isDominator,
        navigate,
        nodeName,
        onBackToTimeline,
      } = vnode.attrs;

      const dataSource = datasourceMemo.use({
        key: {pathHashes, isDominator: isDominator ?? false},
        compute: () => {
          if (!pathHashes) return null;
          const query = flamegraphQuery(pathHashes, isDominator ?? false);
          counter.init(engine, query, SQL_PREAMBLE);
          return new SQLDataSource({
            engine,
            tableOrSubquery: query,
            preamble: SQL_PREAMBLE,
          });
        },
      });

      if (!dataSource) {
        return m(
          DetailsShell,
          {
            title: nodeName ? `Flamegraph: ${nodeName}` : 'Flamegraph Objects',
            fillHeight: true,
            className: 'pf-hde-tab--padded',
          },
          m(
            'div',
            {class: 'pf-hde-card pf-hde-mb-3'},
            m(
              'p',
              'No flamegraph selection found. Select a node in the ',
              'flamegraph and choose "Open in Heapdump Explorer" to see objects here.',
            ),
          ),
        );
      }

      return m(
        DetailsShell,
        {
          title: counter.heading(
            nodeName ? `Flamegraph: ${nodeName}` : 'Flamegraph Objects',
          ),
          fillHeight: true,
          buttons: onBackToTimeline
            ? m(
                Anchor,
                {class: 'pf-hde-download-link', onclick: onBackToTimeline},
                'Back to Timeline',
              )
            : null,
        },
        m(DataGrid, {
          schema: makeUiSchema(navigate),
          data: dataSource,
          fillHeight: true,
          initialColumns: [
            {id: 'id', field: 'id'},
            {id: 'cls', field: 'cls'},
            {id: 'self_size', field: 'self_size'},
            {id: 'native_size', field: 'native_size'},
            {id: 'retained', field: 'retained'},
            {id: 'retained_native', field: 'retained_native'},
            {id: 'retained_count', field: 'retained_count'},
            {id: 'reachable_size', field: 'reachable_size'},
            {id: 'reachable_native', field: 'reachable_native'},
            {id: 'reachable_count', field: 'reachable_count'},
            {id: 'heap', field: 'heap'},
          ],
          showExportButton: true,
          onFiltersChanged: counter.onFiltersChanged,
        }),
      );
    },
  };
}
