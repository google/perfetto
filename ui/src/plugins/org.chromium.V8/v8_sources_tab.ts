import m from 'mithril';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {Filter} from '../../components/widgets/datagrid/model';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {SQLSchemaRegistry} from '../../components/widgets/datagrid/sql_schema';
import {Trace} from '../../public/trace';
import {Editor} from '../../widgets/editor';
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
    table: 'v8_js_script',
    columns: {
      v8_js_script_id: {},
      name: {},
      script_type: {},
      source: {},
      v8_isolate_id: {},
      script_size: {
        expression: (alias) => `LENGTH(${alias}.source)`,
      },
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

export class V8SourcesTab implements Tab {
  private currentTab = TAB_SOURCE;
  private selectedScriptSource: string | undefined = undefined;
  private selectedScriptDetails: V8JsScript | undefined = undefined;
  private trace: Trace;
  private dataSource: SQLDataSource;
  private filters: readonly Filter[] = [];
  private isReady = false;

  constructor(trace: Trace) {
    this.trace = trace;
    this.dataSource = new SQLDataSource({
      engine: this.trace.engine,
      sqlSchema: V8_JS_SCRIPT_SCHEMA,
      rootSchemaName: V8_JS_SCRIPT_SCHEMA_NAME,
    });
    this.initialize();
  }

  private async initialize() {
    await this.trace.engine.query(`INCLUDE PERFETTO MODULE v8.jit;`);
    this.isReady = true;
    m.redraw();
  }

  getTitle(): string {
    return 'V8 Script Sources';
  }

  private async showSourceForScript(id: number) {
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

  private renderSourceTab() {
    return m(Editor, {
      text: this.selectedScriptSource,
      language: 'javascript',
      readonly: true,
    });
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

  private renderTabContent() {
    if (this.currentTab === TAB_SOURCE) {
      return this.renderSourceTab();
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
