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
import {DataGridConfigSchema} from './schema';
import {z} from 'zod';

class TestDataGridPage implements m.ClassComponent {
  private jsCode = `({
  schema: (() => {
    const root = {
      id: {},
      ts: {},
      dur: {},
      name: {},
      track_id: {},
      track: {
        schema: {
          id: {},
          name: {},
        },
      },
      arg: {
        name: 'Argument',
        parameterized: true,
      },
      parent: {
        name: 'Parent',
        get schema() {
          return root;
        },
      },
    };
    return root;
  })(),
  sql: (() => {
    const sqlSchema = {
      sql: 'select * from slice',
      schema: {
        track: {
          join: (ctx) => \`left join track as \${ctx.tableAlias} on \${ctx.tableAlias}.id = base.track_id\`,
        },
        arg: {
          select: (param, ctx) => \`extract_arg(\${ctx.parentAlias}.arg_set_id, '\${param}')\`,
        },
        parent: {
          get schema() {
            return sqlSchema.schema;
          },
          join: (ctx) => {
            const parentAlias = ctx.path.length === 1 ? 'base' : ctx.parentAlias;
            return \`left join slice as \${ctx.tableAlias} on \${ctx.tableAlias}.id = \${parentAlias}.parent_id\`;
          },
        },
      },
    };
    return sqlSchema;
  })(),
  cols: [
    { field: ['id'], id: 'id_1', colId: 'id' },
    { field: ['ts'], id: 'ts_1', colId: 'ts' },
    { field: ['name'], id: 'name_1', colId: 'name' },
    { field: ['track', 'name'], id: 'track_name_1', colId: 'track_name' },
    { field: ['parent', 'name'], id: 'parent_name_1', colId: 'parent_name' },
    { field: ['arg', 'some_argkey'], id: 'some_argkey_1', colId: 'some_argkey' },
    { field: ['dur'], id: 'dur_1', colId: 'dur' },
  ],
  pivot: {
    groupby: ['track_name'],
    aggregate: [
      { colId: 'dur', func: 'sum' },
    ],
  },
})`;
  private columns: ColumnMetadata[] = [];
  private schemaObj: Record<string, unknown> = {};
  private baseSql = '';
  private sqlObj: Record<string, unknown> = {};
  private pivotObj?: unknown = undefined;
  private errorMsg = '';

  constructor() {
    this.evalCode(this.jsCode);
  }

  private formatZodError(error: z.ZodError): string {
    return error.issues
      .map((issue) => {
        const path = issue.path.join('.') || 'root';
        return `❌ [${path}]: ${issue.message}`;
      })
      .join('\n');
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

      let config: any;
      let hasConfig = false;

      try {
        const parseResult = DataGridConfigSchema.safeParse(result);
        if (parseResult.success) {
          config = parseResult.data;
          hasConfig = true;
        } else {
          this.errorMsg = 'Schema Validation Error:\n' + this.formatZodError(parseResult.error);
          console.warn('TestDataGrid schema validation failed:', parseResult.error.format());
        }
      } catch (err) {
        const error = err as Error;
        if (error.message.includes('Maximum call stack size exceeded')) {
          console.warn('TestDataGrid validation bypassed due to cyclic schema:', error);
          config = result;
          hasConfig = true;
        } else {
          throw err;
        }
      }

      if (hasConfig) {
        const schemaObj = config.schema as Record<string, unknown>;
        this.schemaObj = schemaObj;

        this.columns = config.cols.map((col: any) => ({
          key: col.id,
          colId: col.colId,
          path: col.field,
          displayNameParts: resolveDisplayNameParts(col.field, schemaObj),
        }));

        if (config.sql) {
          this.sqlObj = config.sql;
          this.baseSql = config.sql.sql || '';
        } else {
          this.sqlObj = {};
          this.baseSql = '';
        }

        this.pivotObj = config.pivot;
        this.errorMsg = '';
        console.log('Initial columns populated:', this.columns.map(c => c.key));
        return;
      }
      
      // Reset columns and baseSql if validation fails
      this.columns = [];
      this.schemaObj = {};
      this.baseSql = '';
      this.sqlObj = {};
      this.pivotObj = undefined;
    } catch (err) {
      const error = err as Error;
      this.errorMsg = 'JS Evaluation Error:\n' + error.message;
      console.error('TestDataGrid eval error:', err);
      
      this.columns = [];
      this.schemaObj = {};
      this.baseSql = '';
      this.sqlObj = {};
      this.pivotObj = undefined;
    }
  }

  private renderSqlContent(): m.Children {
    if (this.errorMsg) {
      return m('pre.pf-test-sql-error', this.errorMsg);
    }
    try {
      const sql = generateSqlQuery(this.baseSql, this.columns, this.sqlObj, this.pivotObj);
      return m('pre.pf-test-sql-code', sql);
    } catch (err) {
      const error = err as Error;
      return m('pre.pf-test-sql-error', `SQL Generation Error:\n${error.message}`);
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
              this.renderSqlContent(),
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
