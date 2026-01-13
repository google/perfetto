import m from 'mithril';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {Filter} from '../../components/widgets/datagrid/model';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {SQLSchemaRegistry} from '../../components/widgets/datagrid/sql_schema';
import {Trace} from '../../public/trace';
import {Editor} from '../../widgets/editor';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {TextInput} from '../../widgets/text_input';
import {Tree, TreeNode} from '../../widgets/tree';
import {TabStrip} from '../../widgets/tabs';
import {Row, SqlValue} from '../../trace_processor/query_result';
import {NUM, STR} from '../../trace_processor/query_result';
import {CopyableLink} from '../../widgets/copyable_link';
import {Tab} from '../../public/tab';
import {Anchor} from '../../widgets/anchor';
import {Spinner} from '../../widgets/spinner';
import {SplitPanel} from '../../widgets/split_panel';
import { prettyPrint } from './pretty_print_utils';
import { threadStateIdColumn } from 'src/components/widgets/sql/table/columns';

interface V8JsScript {
  v8_js_script_id: number;
  name: string;
  script_type: string;
  source: string;
  v8_isolate_id: number;
  script_size: number;
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

const UNIT_SIZE = 1024;
const UNITS = ['B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB', 'EiB', 'ZiB', 'YiB'];
function formatByteValue(value: SqlValue): string {
  if (typeof value !== 'bigint') {
    return String(value);
  }
  let converted = Number(value);
  let unitIndex = 0;
  while (converted >= UNIT_SIZE && unitIndex < UNITS.length - 1) {
    converted /= UNIT_SIZE;
    unitIndex++;
  }
  return `${converted.toFixed(2)} ${UNITS[unitIndex]}`;
}

const TAB_SOURCE = 'source';
const TAB_DETAILS = 'details';
const TAB_FUNCTIONS = 'functions';

export class V8SourcesTab implements Tab {
  private currentTab = TAB_SOURCE;
  private selectedScriptSource: string = "";
  private selectedScriptDetails: V8JsScript | undefined = undefined;
  private trace: Trace;
  private dataSource: SQLDataSource;
  private filters: readonly Filter[] = [];
  private functionsDataSource: SQLDataSource;
  private functionsFilters: readonly Filter[] = [];
  private isReady = false;
  private formattedScriptSource: string = "";
  private showPrettyPrinted = false;
  private formattedScriptSourceMap: Int32Array | undefined = undefined;

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
    m.redraw();
  }

  getTitle(): string {
    return 'V8 Script Sources';
  }

  private async showSourceForScript(id: number) {
    this.showPrettyPrinted = false;
    this.formattedScriptSource = "";
    this.formattedScriptSourceMap = undefined;
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
    if (it.valid()) {
      this.selectedScriptSource = it.source as string;
      this.selectedScriptDetails = {
        v8_js_script_id: it.v8_js_script_id as number,
        name: it.name as string,
        script_type: it.script_type as string,
        source: it.source as string,
        v8_isolate_id: it.v8_isolate_id as number,
        script_size: it.script_size as number,
      };
      this.functionsFilters = [
        {
          field: 'v8_js_script_id',
          op: '=',
          value: id,
        },
      ];
    }
    m.redraw();
  }

  filterScript(searchTerm: string) {
    if (searchTerm) {
      this.filters = [
        {
          field: 'name',
          op: 'glob',
          value: `*${searchTerm}*`,
        },
      ];
    } else {
      this.filters = [];
    }
    m.redraw();
  }

  private async togglePrettyPrint() {
    this.showPrettyPrinted = !this.showPrettyPrinted ;
    if (this.showPrettyPrinted && !this.formattedScriptSource) {
      try {
        const {formatted, sourceMap} = await prettyPrint(this.selectedScriptSource);
        this.formattedScriptSource = formatted;
        this.formattedScriptSourceMap = sourceMap;
      } catch (e) {
        console.error('Pretty print failed', e);
        return;
      }
    }
    m.redraw();
  }

  public get scriptSource() : string {
    return this.showPrettyPrinted
      ? this.formattedScriptSource
      : this.selectedScriptSource;
  }

  mapSourcePosition(originalPos: number): number {
    if (!this.formattedScriptSourceMap) return originalPos;
    // If the exact position is not mapped (e.g. whitespace), find the next mapped position.
    for (let i = originalPos; i < this.formattedScriptSourceMap.length; i++) {
      if (this.formattedScriptSourceMap[i] !== -1) {
        return this.formattedScriptSourceMap[i];
      }
    }
    // If not found (e.g. trailing whitespace), return the end of formatted string or last mapped.
    return this.formattedScriptSource?.length ?? 0;
  }


  private renderSourceTab() {
    return m(
      '.pf-v8-source-container',
      {
        style: {
          position: 'relative',
          height: '100%',
        },
      },
      m(Editor, {
        text: this.scriptSource,
        language: 'javascript',
        readonly: true,
        fillHeight: true,
      }),
      m(
        '.pf-v8-floating-toolbar',
        {
          style: {
            position: 'absolute',
            bottom: '20px',
            right: '20px',
            zIndex: 10,
          },
        },
        m(Button, {
          icon: 'data_object',
          title: this.showPrettyPrinted ? 'Show Original' : 'Pretty Print',
          variant: ButtonVariant.Filled,
          intent: this.showPrettyPrinted ? Intent.Primary : undefined,
          active: this.showPrettyPrinted,
          onclick: () => this.togglePrettyPrint(),
        }),
      ),
    );
  }

  private renderDetailsTab() {
    if (!this.selectedScriptDetails) {
      return m('div', 'No script selected');
    }

    return m(
      Tree,
      m(TreeNode, {
        left: 'ID',
        right: String(this.selectedScriptDetails.v8_js_script_id),
      }),
      m(TreeNode, {
        left: 'Name',
        right: this.selectedScriptDetails.name.startsWith('http')
          ? m(CopyableLink, {url: this.selectedScriptDetails.name})
          : this.selectedScriptDetails.name,
      }),
      m(TreeNode, {
        left: 'Type',
        right: this.selectedScriptDetails.script_type,
      }),
      m(TreeNode, {
        left: 'Isolate',
        right: String(this.selectedScriptDetails.v8_isolate_id),
      }),
      m(TreeNode, {
        left: 'Size',
        right: formatByteValue(BigInt(this.selectedScriptDetails.script_size)),
      }),
    );
  }

  private renderFunctionsTab() {
    if (!this.selectedScriptDetails) {
      return m('div', 'No script selected');
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

  private renderTabContent() {
    if (this.currentTab === TAB_SOURCE) {
      return this.renderSourceTab();
    }
    if (this.currentTab === TAB_FUNCTIONS) {
      return this.renderFunctionsTab();
    }
    return this.renderDetailsTab();
  }

  render() {
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
                  this.showSourceForScript(row.v8_js_script_id as number);
                },
              },
              String(value),
            );
          },
        },
        name: {
          title: 'Name',
          cellRenderer: (value: unknown, row: Row) => {
            if (typeof value !== 'string') {
              return undefined;
            }
            if (!value.startsWith('http')) {
              return String(value);
            }
            return m(CopyableLink, {
              url: String(row.name),
            });
          },
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
            this.filterScript(searchTerm);
          },
          placeholder: 'Search scripts',
          leftIcon: 'search',
        }),
        m(DataGrid, {
          data: this.dataSource,
          schema: v8JsScriptUiSchema,
          rootSchema: V8_JS_SCRIPT_SCHEMA_NAME,
          filters: this.filters,
          onFiltersChanged: (filters: readonly Filter[]) => {
            this.filters = filters;
            m.redraw();
          },
          initialColumns: [
            {field: 'v8_js_script_id', id: 'v8_js_script_id'},
            {field: 'script_size', id: 'script_size'},
            {field: 'name', id: 'name'},
          ],
        }),
      ),
      secondPanel: m(
        '.pf-v8-source-script-details',
        m(TabStrip, {
          tabs: [
            {key: TAB_SOURCE, title: 'Source'},
            {key: TAB_FUNCTIONS, title: 'Functions'},
            {key: TAB_DETAILS, title: 'Details'},
          ],
          currentTabKey: this.currentTab,
          onTabChange: (key) => {
            this.currentTab = key;
            m.redraw();
          },
        }),
        m('.pf-tab-page', this.renderTabContent()),
      ),
    });
  }
}