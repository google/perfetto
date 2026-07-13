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

import './styles.scss';
import m from 'mithril';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import {SplitPanel} from '../../widgets/split_panel';
import {Editor} from '../../widgets/editor';
import {generateSqlQuery, type ColumnMetadata} from './sql_generator';
import {DataGrid, resolveDisplayNameParts} from './datagrid';

class TestDataGridPage implements m.ClassComponent {
  private jsCode = `({
  schema: (() => {
    const node = {
      'foo': {
        schema: {
          'bar': { name: 'Nested Bar Column' },
          'raw_nested': {},
          'arg': {
            name: 'Argument',
            parameterized: true,
          },
        },
      },
      'simple_col': {},
      'baz': {
        schema: {
          'inner': {
            schema: {
              'deep': {},
            },
          },
        },
      },
    };
    const root = {
      ...node,
      'recursive_node': {
        schema: {},
      },
    };
    root.recursive_node.schema = root; // Recursive reference!
    return root;
  })(),
  cols: [
    { field: ['foo', 'bar'], id: 'foo_bar' },
    { field: ['foo', 'raw_nested'], id: 'foo_raw_nested' },
    { field: ['simple_col'], id: 'simple_col' },
    { field: ['baz', 'inner', 'deep'], id: 'baz_deep' },
    { field: ['recursive_node', 'recursive_node', 'foo', 'bar'], id: 'rec_foo_bar' },
    { field: ['foo', 'arg', 'some_argkey'], id: 'some_argkey' },
  ],
  sql: (() => {
    const sqlSchema = {
      'foo': {
        join: (ctx) => \`left join foo_table as \${ctx.tableAlias} on \${ctx.tableAlias}.id = base.foo_id\`,
        schema: {
          'arg': {
            select: (param, ctx) => \`extract_arg(\${ctx.parentAlias}.arg_set_id, '\${param}')\`,
          },
        },
      },
      'baz': {
        join: (ctx) => \`left join baz_table as \${ctx.tableAlias} using (baz_id)\`,
        schema: {
          'inner': {
            join: (ctx) => \`left join inner_table as \${ctx.tableAlias} on \${ctx.tableAlias}.baz_id = baz_table.id\`,
          },
        },
      },
      'recursive_node': {
        join: (ctx) => {
          const parentAlias = ctx.path.length === 1 ? 'base' : ctx.parentAlias;
          return \`left join recursive_table as \${ctx.tableAlias} on \${ctx.tableAlias}.id = \${parentAlias}.parent_id\`;
        },
        schema: {},
      },
    };
    sqlSchema.recursive_node.schema = sqlSchema; // Recursive reference!
    return {
      sql: 'select * from slice',
      schema: sqlSchema,
    };
  })()
})`;
  private columns: ColumnMetadata[] = [];
  private schemaObj: Record<string, unknown> = {};
  private baseSql = '';
  private sqlObj: Record<string, unknown> = {};

  constructor() {
    this.evalCode(this.jsCode);
  }

  private evalCode(val: string) {
    try {
      let codeToEval = val.trim();
      // Simple heuristic to wrap raw object literals in parentheses
      if (codeToEval.startsWith('{') && codeToEval.endsWith('}')) {
        codeToEval = `(${codeToEval})`;
      }
      const result = eval(codeToEval);
      console.log('Evaluated result:', result);

      // Extract schema if present
      if (result && typeof result === 'object' && 'schema' in result) {
        const schema = (result as {schema?: unknown}).schema;
        const cols = (result as {cols?: unknown}).cols;
        const sqlVal = (result as {sql?: unknown}).sql;

        if (schema && typeof schema === 'object') {
          const schemaObj = schema as Record<string, unknown>;
          this.schemaObj = schemaObj;

          if (Array.isArray(cols)) {
            this.columns = cols
              .map((colEntry) => {
                if (!colEntry || typeof colEntry !== 'object') return null;
                const field = (colEntry as {field?: unknown}).field;
                const id = (colEntry as {id?: unknown}).id;

                if (!Array.isArray(field)) return null;
                if (!field.every((item) => typeof item === 'string'))
                  return null;
                if (typeof id !== 'string') return null;

                const displayNameParts = resolveDisplayNameParts(
                  field,
                  schemaObj,
                );

                return {
                  key: id,
                  path: field,
                  displayNameParts,
                };
              })
              .filter((col): col is ColumnMetadata => col !== null);
          } else {
            this.columns = [];
          }

          if (sqlVal && typeof sqlVal === 'object') {
            this.sqlObj = sqlVal;
            const innerSql = (sqlVal as {sql?: unknown}).sql;
            if (typeof innerSql === 'string') {
              this.baseSql = innerSql;
            } else {
              this.baseSql = '';
            }
          } else {
            this.sqlObj = {};
            this.baseSql = '';
          }
          console.log(
            'Initial columns populated:',
            this.columns.map((c) => c.key),
          );
          return;
        }
      }
      // Reset columns and baseSql if no valid schema returned
      this.columns = [];
      this.schemaObj = {};
      this.baseSql = '';
      this.sqlObj = {};
    } catch (err) {
      // Log errors to console so we can see issues (e.g. syntax errors or missing params)
      console.error('TestDataGrid eval error:', err);
    }
  }

  view() {
    return m(
      '.pf-test-datagrid-page',
      m('h1', 'TestDataGrid Page'),
      m('p', 'This page is used for testing purposes.'),
      m(
        '.pf-test-datagrid-split-container',
        m(SplitPanel, {
          direction: 'horizontal',
          initialSplit: {percent: 50},
          minSize: 150,
          firstPanel: m(Editor, {
            text: this.jsCode,
            fillHeight: true,
            onUpdate: (val) => {
              this.jsCode = val;
              this.evalCode(val);
            },
          }),
          secondPanel: m(SplitPanel, {
            direction: 'vertical',
            initialSplit: {percent: 60},
            minSize: 100,
            firstPanel: m(
              '.pf-test-grid-container',
              m(DataGrid, {
                schema: this.schemaObj,
                columns: this.columns,
                onColumnsChanged: (cols) => {
                  this.columns = cols;
                },
              }),
            ),
            secondPanel: m(
              '.pf-test-sql-container',
              m(
                'pre.pf-test-sql-code',
                generateSqlQuery(this.baseSql, this.columns, this.sqlObj),
              ),
            ),
          }),
        }),
      ),
    );
  }
}

export default class implements PerfettoPlugin {
  static readonly id = 'test.DataGrid';

  static onActivate(app: App): void {
    app.pages.registerPage({
      route: '/test_datagrid',
      render: () => m(TestDataGridPage),
    });
    app.sidebar.addMenuItem({
      section: 'settings',
      text: 'Test DataGrid',
      href: '#!/test_datagrid',
      icon: 'grid_on',
      sortOrder: 100,
    });
  }
}
