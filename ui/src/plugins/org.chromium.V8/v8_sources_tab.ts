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
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {Filter} from '../../components/widgets/datagrid/model';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {SQLSchemaRegistry} from '../../components/widgets/datagrid/sql_schema';
import {Tab} from '../../public/tab';
import {Trace} from '../../public/trace';
import {NUM, Row, SqlValue, STR} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {CopyableLink} from '../../widgets/copyable_link';
import {Editor} from '../../widgets/editor';
import {EmptyState} from '../../widgets/empty_state';
import {Spinner} from '../../widgets/spinner';
import {SplitPanel} from '../../widgets/split_panel';
import {Tabs} from '../../widgets/tabs';
import {TextInput} from '../../widgets/text_input';
import {Tree, TreeNode} from '../../widgets/tree';
import {formatFileSize} from '../../base/file_utils';
import {QuerySlot, SerialTaskQueue} from '../../base/query_slot';

interface V8JsScript {
  v8_js_script_id: number;
  name: string;
  script_type: string;
  source: string;
  v8_isolate_id: number;
  script_size: number;
}

interface ScriptResults {
  source: string;
  details: V8JsScript;
}

const V8_JS_SCRIPT_SCHEMA_NAME = 'v8JsScript';
const V8_JS_SCRIPT_SCHEMA: SQLSchemaRegistry = {
  [V8_JS_SCRIPT_SCHEMA_NAME]: {
    table: 'v8_js_script_view',
    columns: {
      v8_js_script_id: {},
      name: {},
      domain: {},
      script_type: {},
      source: {},
      v8_isolate_id: {},
      script_size: {
        expression: (alias) => `LENGTH(${alias}.source)`,
      },
    },
  },
};

const V8_JS_FUNCTION_SCHEMA_NAME = 'v8JsFunction';
const V8_JS_FUNCTION_SCHEMA: SQLSchemaRegistry = {
  [V8_JS_FUNCTION_SCHEMA_NAME]: {
    table: 'v8_js_function',
    columns: {
      v8_js_function_id: {},
      name: {},
      v8_js_script_id: {},
      is_toplevel: {},
      kind: {},
      line: {},
      col: {},
    },
  },
};

function formatByteValue(value: SqlValue): string {
  if (typeof value !== 'bigint') {
    return String(value);
  }
  return formatFileSize(value);
}

function formatUrlValue(value: SqlValue): string | m.Children {
  if (typeof value !== 'string') {
    return undefined;
  }
  if (!value.startsWith('http')) {
    return String(value);
  }
  return m(CopyableLink, {
    url: String(value),
  });
}

const TAB_SOURCE = 'source';
const TAB_DETAILS = 'details';
const TAB_FUNCTIONS = 'functions';

export class V8SourcesTab implements Tab {
  private currentTab = TAB_SOURCE;
  private readonly slot = new QuerySlot<ScriptResults | undefined>(
    new SerialTaskQueue(),
  );
  private selectedScriptId: number | undefined = undefined;
  private trace: Trace;
  private dataSource: SQLDataSource;
  private filters: readonly Filter[] = [];
  private functionsDataSource: SQLDataSource;
  private functionsFilters: readonly Filter[] = [];
  private isReady = false;

  constructor(trace: Trace) {
    this.trace = trace;
    this.dataSource = new SQLDataSource({
      engine: this.trace.engine,
      sqlSchema: V8_JS_SCRIPT_SCHEMA,
      rootSchemaName: V8_JS_SCRIPT_SCHEMA_NAME,
    });
    this.functionsDataSource = new SQLDataSource({
      engine: this.trace.engine,
      sqlSchema: V8_JS_FUNCTION_SCHEMA,
      rootSchemaName: V8_JS_FUNCTION_SCHEMA_NAME,
    });
    this.initialize();
  }

  private async initialize() {
    await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE v8.jit;
      CREATE VIEW IF NOT EXISTS v8_js_script_view AS
      SELECT *,
        CASE
          WHEN name GLOB 'chrome://*' THEN 'chrome'
          WHEN name GLOB 'v8/*' THEN 'v8'
          WHEN name GLOB 'extensions::*' THEN 'extensions'
          ELSE STR_SPLIT(name, '/', 2)
        END AS domain
      FROM v8_js_script;
    `);
    this.isReady = true;
  }

  getTitle(): string {
    return 'V8 Script Sources';
  }

  private async selectScript(id: number | undefined) {
    if (id === undefined) return undefined;
    const queryResult = await this.trace.engine.query(
      `INCLUDE PERFETTO MODULE v8.jit;
       SELECT *, LENGTH(source) AS script_size
       FROM v8_js_script
       WHERE v8_js_script_id = ${id}`,
    );
    const it = queryResult.iter({
      v8_js_script_id: NUM,
      name: STR,
      script_type: STR,
      source: STR,
      v8_isolate_id: NUM,
      script_size: NUM,
    });
    if (!it.valid()) return undefined;
    this.functionsFilters = [
      {
        field: 'v8_js_script_id',
        op: '=',
        value: id,
      },
    ];
    return {
      source: it.source as string,
      details: {
        v8_js_script_id: it.v8_js_script_id as number,
        name: it.name as string,
        script_type: it.script_type as string,
        source: it.source as string,
        v8_isolate_id: it.v8_isolate_id as number,
        script_size: it.script_size as number,
      },
    };
  }

  filterScripts(searchTerm: string) {
    if (!searchTerm) {
      this.filters = [];
    } else {
      this.filters = [
        {
          field: 'name',
          op: 'glob',
          value: `*${searchTerm}*`,
        },
      ];
    }
    m.redraw();
  }

  private renderSourceTab(source: string) {
    return m(Editor, {
      text: source,
      language: 'javascript',
      readonly: true,
    });
  }

  private renderDetailsTab(scriptDetails?: V8JsScript) {
    if (!scriptDetails) {
      return undefined;
    }

    return m(
      Tree,
      m(TreeNode, {
        left: 'ID',
        right: String(scriptDetails.v8_js_script_id),
      }),
      m(TreeNode, {
        left: 'Name',
        right: formatUrlValue(scriptDetails.name),
      }),
      m(TreeNode, {
        left: 'Type',
        right: scriptDetails.script_type,
      }),
      m(TreeNode, {
        left: 'Isolate',
        right: String(scriptDetails.v8_isolate_id),
      }),
      m(TreeNode, {
        left: 'Size',
        right: formatByteValue(BigInt(scriptDetails.script_size)),
      }),
    );
  }

  private renderFunctionsTab(scriptDetails?: V8JsScript) {
    if (!scriptDetails) {
      return undefined;
    }
    const v8JsFunctionUiSchema: SchemaRegistry = {
      v8JsFunction: {
        v8_js_function_id: {title: 'ID'},
        name: {title: 'Name'},
        is_toplevel: {title: 'Is Toplevel'},
        kind: {title: 'Kind'},
        line: {title: 'Line'},
        col: {title: 'Column'},
      },
    };
    return m(DataGrid, {
      data: this.functionsDataSource,
      schema: v8JsFunctionUiSchema,
      rootSchema: V8_JS_FUNCTION_SCHEMA_NAME,
      fillHeight: true,
      initialFilters: this.functionsFilters,
      onFiltersChanged: (filters: readonly Filter[]) => {
        this.functionsFilters = filters;
        m.redraw();
      },
      initialColumns: [
        {field: 'v8_js_function_id', id: 'v8_js_function_id'},
        {field: 'name', id: 'name'},
        {field: 'kind', id: 'kind'},
        {field: 'line', id: 'line'},
        {field: 'col', id: 'col'},
      ],
    });
  }

  render() {
    const selectedId = this.selectedScriptId;
    const {data: scriptResult} = this.slot.use({
      key: {id: selectedId},
      retainOn: ['id'],
      queryFn: () => this.selectScript(selectedId),
    });

    const v8JsScriptUiSchema: SchemaRegistry = {
      v8JsScript: {
        v8_js_script_id: {
          title: 'ID',
          cellRenderer: (value: unknown, row: Row) => {
            return m(
              Anchor,
              {
                onclick: (e: Event) => {
                  e.preventDefault();
                  this.selectedScriptId = row.v8_js_script_id as number;
                },
              },
              String(value),
            );
          },
        },
        name: {
          title: 'Name',
          cellRenderer: formatUrlValue,
        },
        domain: {
          title: 'Domain',
        },
        source: {
          title: 'Source',
        },
        script_type: {
          title: 'Type',
        },
        v8_isolate_id: {
          title: 'Isolate',
        },
        script_size: {
          title: 'Size',
          cellRenderer: formatByteValue,
        },
      },
    };

    if (!this.isReady) {
      return m('.pf-v8-loading-container', m(Spinner));
    }

    return m(SplitPanel, {
      className: 'pf-v8-source-view',
      direction: 'horizontal',
      initialSplit: {pixels: 400},
      controlledPanel: 'first',
      firstPanel: m(
        '.pf-script-list-pane',
        m(TextInput, {
          oninput: (e: Event) => {
            const searchTerm = (e.target as HTMLInputElement).value;
            this.filterScripts(searchTerm);
          },
          placeholder: 'Search scripts',
          leftIcon: 'search',
        }),
        m(DataGrid, {
          data: this.dataSource,
          schema: v8JsScriptUiSchema,
          rootSchema: V8_JS_SCRIPT_SCHEMA_NAME,
          fillHeight: true,
          filters: this.filters,
          onFiltersChanged: (filters: readonly Filter[]) => {
            this.filters = filters;
            m.redraw();
          },
          initialColumns: [
            {field: 'v8_js_script_id', id: 'v8_js_script_id'},
            {field: 'script_size', id: 'script_size', sort: 'DESC'},
            {field: 'name', id: 'name'},
          ],
        }),
      ),
      secondPanel: m(
        '.pf-v8-source-script-details',
        !scriptResult
          ? m(
              EmptyState,
              {
                fillHeight: true,
                icon: 'no_sim',
                title: 'No script selected',
              },
              'Select a script from the list to view details.',
            )
          : m(Tabs, {
              tabs: [
                {
                  key: TAB_SOURCE,
                  title: 'Source',
                  leftIcon: 'code',
                  content: this.renderSourceTab(scriptResult.source),
                },
                {
                  key: TAB_FUNCTIONS,
                  title: 'Functions',
                  leftIcon: 'function',
                  content: this.renderFunctionsTab(scriptResult.details),
                },
                {
                  key: TAB_DETAILS,
                  title: 'Details',
                  leftIcon: 'info',
                  content: this.renderDetailsTab(scriptResult.details),
                },
              ],
              activeTabKey: this.currentTab,
              onTabChange: (key) => {
                this.currentTab = key;
                m.redraw();
              },
            }),
      ),
    });
  }
}
