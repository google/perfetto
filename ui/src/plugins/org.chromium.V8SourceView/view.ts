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
      id: {column: 'v8_js_script_id'},
      name: {},
    },
  },
};


export class V8SourceView implements m.ClassComponent<{trace: Trace}> {
  private selectedScriptSource: string|undefined = undefined;
  private selectedScriptId: number|undefined = undefined;
  private trace!: Trace;
  private dataSource!: SQLDataSource;

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
    const filters: Filter[] = [];
    if (searchTerm) {
      filters.push({
        field: 'name',
        op: 'glob',
        value: `*${searchTerm}*`,
      });
    }
    this.dataSource.notify({filters});
    m.redraw();
  }

  view() {
    const v8JsScriptUiSchema: SchemaRegistry = {
      v8JsScript: {
        id: {
          title: 'ID',
          cellRenderer: (value, row: Row): CellRenderResult => {
            return {
              content: renderCell(value, 'id'),
              className: row.id === this.selectedScriptId ? 'pf-highlight-row' : undefined,
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
                      this.showSourceForScript(row.id as number);
                    },
                  },
                  renderCell(value, 'name')),
              className: row.id === this.selectedScriptId ? 'pf-highlight-row' : undefined,
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
            initialColumns: [
              {field: 'id'},
              {field: 'name'},
            ],
          })),
        m('.pf-source-view',
          m(Editor, {text: this.selectedScriptSource, readonly: true}),
        ));
  }
}
