import m from 'mithril';
import {DataGrid, renderCell} from '../../components/widgets/datagrid/datagrid';
import {
  CellRenderResult,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {Filter} from '../../components/widgets/datagrid/model';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {SQLSchemaRegistry} from '../../components/widgets/datagrid/sql_schema';
import {Trace} from '../../public/trace';
import {Editor} from '../../widgets/editor';
import {TextInput} from '../../widgets/text_input';
import {Row} from '../../trace_processor/query_result';

const V8_JS_SCRIPT_SCHEMA_NAME = 'v8JsScript';
const V8_JS_SCRIPT_SCHEMA: SQLSchemaRegistry = {
  [V8_JS_SCRIPT_SCHEMA_NAME]: {
    table: 'v8_js_script',
    columns: {
      v8_js_script_id: {},
      name: {},
      script_type: { },
      source: {},
      v8_isolate_id: { },
      script_size: {
        expression: (alias) => `LENGTH(${alias}.source)`,
      },
    },
  },
};


export class V8SourceView implements m.ClassComponent<{trace: Trace}> {
  private selectedScriptSource: string|undefined = undefined;
  private selectedScriptId: number|undefined = undefined;
  private trace!: Trace;
  private dataSource!: SQLDataSource;
  private filters: Filter[] = [];

  oninit(vnode: m.Vnode<{trace: Trace}>) {
    this.trace = vnode.attrs.trace;
    this.dataSource = new SQLDataSource({
      engine: this.trace.engine,
      sqlSchema: V8_JS_SCRIPT_SCHEMA,
      rootSchemaName: V8_JS_SCRIPT_SCHEMA_NAME,
    });
  }

  private async showSourceForScript(id: number) {
    this.selectedScriptId = id;
    const queryResult = await this.trace.engine.query(
        `select source from v8_js_script where v8_js_script_id = ${id}`);
    const it = queryResult.iter({source: 'str'});
    if (it.valid()) {
      this.selectedScriptSource = it.source;
    }
    m.redraw();
  }

  filterScript(searchTerm: string) {
    if (searchTerm) {
      this.filters = [{
        field: 'name',
        op: 'glob',
        value: `*${searchTerm}*`,
      }];
    } else {
      this.filters = [];
    }
    m.redraw();
  }

  scriptHighlightClass(rowId:number) {
    return rowId === this.selectedScriptId ? 'pf-highlight-row' : undefined;
  }

  view() {
    const v8JsScriptUiSchema: SchemaRegistry = {
      v8JsScript: {
        v8_js_script_id: {
          title: 'ID',
          cellRenderer: (value, row: Row): CellRenderResult => {
            return {
              content: renderCell(value, 'v8_js_script_id'),
              className: this.scriptHighlightClass(row.v8_js_script_id as number),
            };
          },
        },
        name: {
          title: 'Name',
          cellRenderer: (value, row: Row): CellRenderResult => {
            return {
              content: m(
                  'a',
                  {
                    href: '#',
                    onclick: (e: Event) => {
                      e.preventDefault();
                      this.showSourceForScript(row.v8_js_script_id as number);
                    },
                  },
                  renderCell(value, 'name')),
              className: this.scriptHighlightClass(row.v8_js_script_id as number),
            };
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
          title: 'Size (KiB)',
          cellRenderer: (value, row: Row): CellRenderResult => {
            const sizeInKiB = Number(value) / 1024;
            return {
              content: renderCell(sizeInKiB.toFixed(2), 'script_size'),
              className: this.scriptHighlightClass(row.v8_js_script_id as number),
              align: 'right',
            };
          },
        },
      },
    };

    return m(
        '.pf-v8-source-view',
        m('.pf-script-list-pane',
          m(TextInput, {
            oninput: (e: Event) => {
              const searchTerm = (e.target as HTMLInputElement).value;
              this.filterScript(searchTerm);
            },
            placeholder: 'Search scripts',
          }),
          m(DataGrid, {
            data: this.dataSource,
            schema: v8JsScriptUiSchema,
            rootSchema: V8_JS_SCRIPT_SCHEMA_NAME,
            filters: this.filters,
            onFiltersChanged: (filters) => {
              this.filters = filters;
            },
            initialColumns: [
              {field: 'v8_js_script_id'},
              {field: 'name'},
              {field: 'script_size'},
            ],
          })),
        m('.pf-source-view',
          m(Editor, {
            text: this.selectedScriptSource,
            language: 'javascript',
            readonly: true,
          }),
        ));
  }
}
