// Copyright (C) 2025 The Android Open Source Project
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
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../../components/widgets/datagrid/sql_data_source';
import {Editor} from '../../../widgets/editor';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Callout} from '../../../widgets/callout';
import {Intent} from '../../../widgets/common';
import {EmptyState} from '../../../widgets/empty_state';
import {Anchor} from '../../../widgets/anchor';
import type {App} from '../../../public/app';
import type {Trace} from '../../../public/trace';
import type {DataGridAttrs} from '../../../components/widgets/datagrid/datagrid';
import {SplitPanel} from '../../../widgets/split_panel';

// The default config shown in the editor. Builds a DataGrid over the slice
// table, joining to `track` and exposing the arg set as a parameterized column.
const DEFAULT_CONFIG = `// Return the DataGrid config: { schema, data, initialColumns }.
//
// In scope:
//   engine         - the current trace's query engine
//   SQLDataSource  - constructs a SQL-backed datasource from a schema
//   m              - the Mithril library, for constructing virtual DOM nodes

// Describes how the columns are displayed in the grid, including in the 'add
// column' menu and defines optional styling overrides.
const schema = {
  id: {title: 'ID', columnType: 'identifier'},
  ts: {title: 'Timestamp', columnType: 'quantitative'},
  dur: {title: 'Duration', columnType: 'quantitative'},
  name: {title: 'Name', columnType: 'text'},
  track: {
    title: 'Track',
    schema: {
      id: {title: 'ID', columnType: 'quantitative'},
      name: {title: 'Name', columnType: 'text'},
    },
  },
  parent: {
    get schema() {
      return schema;
    }
  },
  args: {title: 'Arg', parameterized: true},
};

// Describes the shape of the SQL data: which table, columns, joins and
// parameterized (arg) columns are available.
const sqlSchema = {
  primaryKey: 'id',
  tableOrSubquery: 'slice',
  columns: {
    track: {
      foreignKey: 'track_id',
      schema: {
        tableOrSubquery: 'track',
        columns: {
          id: {},
          name: {},
        },
      },
    },
    parent: {
      foreignKey: 'parent_id',
      get schema() {
        return sqlSchema;
      }
    },
    args: {
      parameterized: true,
      expression: (alias, key) =>
        \`extract_arg(\${alias}.arg_set_id, '\${key}')\`,
      parameterKeysQuery: (tableOrSubquery, alias) => \`
        SELECT DISTINCT args.key
        FROM (\${tableOrSubquery}) AS \${alias}
        JOIN args ON args.arg_set_id = \${alias}.arg_set_id
        WHERE args.key IS NOT NULL
        ORDER BY args.key
        LIMIT 1000
      \`,
    },
  },
};

// To try pivot mode, uncomment this and add \`initialPivot\` to the returned
// object below - groups slices by track name, showing a count and total
// duration per track, in expandable tree form.
// const initialPivot = {
//   groupBy: [{id: 'track_name', field: 'track.name'}],
//   aggregates: [
//     {id: 'count', function: 'COUNT'},
//     {id: 'total_dur', field: 'dur', function: 'SUM'},
//   ],
//   groupDisplay: 'tree',
// };

return {
  schema,
  data: new SQLDataSource({engine, ...sqlSchema}),
  initialColumns: [
    {id: 'id', field: 'id'},
    {id: 'ts', field: 'ts'},
    {id: 'dur', field: 'dur'},
    {id: 'name', field: 'name'},
    {id: 'track_name', field: 'track.name'},
  ],
  // initialPivot,
};
`;

// Result of evaluating the user's config: either the DataGrid attrs to render,
// or an error message to display.
type EvalResult =
  | {readonly ok: true; readonly attrs: DataGridAttrs}
  | {readonly ok: false; readonly error: string};

function evalConfig(trace: Trace, code: string): EvalResult {
  try {
    const fn = new Function('engine', 'SQLDataSource', 'm', code);
    const result: unknown = fn(trace.engine, SQLDataSource, m);
    if (
      result === null ||
      typeof result !== 'object' ||
      !('schema' in result) ||
      !('data' in result)
    ) {
      return {
        ok: false,
        error: 'Config must return an object with `schema` and `data`.',
      };
    }
    return {ok: true, attrs: result as DataGridAttrs};
  } catch (e) {
    return {ok: false, error: `${e}`};
  }
}

// Interactive DataGrid playground: edit a config on the left, see the resulting
// grid on the right. The config is only (re)evaluated on demand (Ctrl/Cmd+Enter
// or the Run button) so editing keystrokes don't rebuild the datasource.
class DataGridDemo implements m.ClassComponent<{trace: Trace}> {
  private text = DEFAULT_CONFIG;
  private result?: EvalResult;
  private generation = 0;
  // The SQL query the grid last asked its datasource to run, kept in sync via
  // the DataGrid's onReady callback. Only available for SQLDataSource - see
  // SQLDataSource.getQuery.
  private query?: string;

  private run(trace: Trace) {
    this.result = evalConfig(trace, this.text);
    this.generation++;
    this.query = undefined;
  }

  view({attrs}: m.Vnode<{trace: Trace}>): m.Children {
    const {trace} = attrs;
    if (this.result === undefined) {
      this.run(trace);
    }
    const result = this.result!;

    // Fill the remaining height as a flex child. `minHeight: 0` is load-bearing:
    // without it this item won't shrink below the fillHeight DataGrid's content
    // size, so the grid keeps growing its own available space and thrashes
    // layout forever. The full-width page chain (see styles.scss) provides the
    // bounded, min-height:0 ancestors this relies on.
    return m(
      '.pf-datagrid-demo',
      {style: {flex: '1', minHeight: 0, marginTop: '16px'}},
      m(SplitPanel, {
        direction: 'horizontal',
        firstPanel: m(
          '.pf-datagrid-demo__editor',
          {
            style: {
              height: '100%',
              display: 'flex',
              flexDirection: 'column',
              minWidth: 0,
            },
          },
          m(
            '.pf-datagrid-demo__toolbar',
            {style: {marginBottom: '4px'}},
            m(Button, {
              variant: ButtonVariant.Filled,
              label: 'Run (Ctrl+Enter)',
              intent: Intent.Primary,
              onclick: () => this.run(trace),
            }),
          ),
          m(
            '.pf-datagrid-demo__editor-container',
            {style: {flex: '1', minHeight: 0}},
            m(Editor, {
              fillHeight: true,
              language: 'javascript',
              text: this.text,
              onUpdate: (text: string) => {
                this.text = text;
              },
              onExecute: (text: string) => {
                this.text = text;
                this.run(trace);
              },
            }),
          ),
        ),
        secondPanel: m(SplitPanel, {
          direction: 'vertical',
          firstPanel: result.ok
            ? m(DataGrid, {
                key: this.generation,
                ...result.attrs,
                fillHeight: true,
                onReady: (api) => {
                  const data = result.ok ? result.attrs.data : undefined;
                  const query =
                    data instanceof SQLDataSource
                      ? data.getQuery(api.getModel())
                      : undefined;
                  if (query !== this.query) {
                    this.query = query;
                    m.redraw();
                  }
                },
              })
            : m(
                Callout,
                {intent: Intent.Danger, icon: 'error'},
                m('pre', result.error),
              ),
          secondPanel: m(Editor, {
            fillHeight: true,
            readonly: true,
            language: 'perfetto-sql',
            text:
              this.query ??
              '-- SQL query not available (needs a SQLDataSource).',
          }),
        }),
      }),
    );
  }
}

export function renderDataGrid(app: App): m.Children {
  const trace = app.trace;
  return m(
    '',
    {
      // Flex column that fills the (bounded) full-width content area. minHeight:0
      // lets it shrink so the DataGridDemo below can own the leftover height
      // without the layout feedback loop.
      style: {
        height: '100%',
        minHeight: 0,
        display: 'flex',
        flexDirection: 'column',
      },
    },
    m(
      '.pf-widget-intro',
      m('h1', 'DataGrid Playground'),
      m('p', [
        'This is an interactive playground. Edit the config on the left to ',
        'define a schema and datasource, then hit Run (or Ctrl/Cmd+Enter) to ',
        'render it in the grid on the right.',
      ]),
      m('p', [
        'See the ',
        m(Anchor, {href: '#!/widgets/datagrid'}, 'DataGrid demo'),
        ' for curated examples of schemas, relationships, and data sources.',
      ]),
    ),
    trace
      ? m(DataGridDemo, {trace})
      : m(
          EmptyState,
          {title: 'Load a trace'},
          'The DataGrid playground needs a trace to query against.',
        ),
  );
}
