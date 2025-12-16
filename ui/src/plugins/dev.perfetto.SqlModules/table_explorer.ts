import m from 'mithril';
import {Tab} from '../../public/tab';
import {
  DataGrid,
  DataGridApi,
  DataGridAttrs,
} from '../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {DetailsShell} from '../../widgets/details_shell';
import {Filter, Column} from '../../components/widgets/datagrid/model';
import {
  getDefaultVisibleFields,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {DataGridExportButton} from '../../components/widgets/datagrid/export_button';
import {AddDebugTrackMenu} from '../../components/tracks/add_debug_track_menu';
import {Button} from '../../widgets/button';
import {PopupMenu} from '../../widgets/menu';
import {PopupPosition} from '../../widgets/popup';
import {Trace} from '../../public/trace';

export class TableExplorer implements Tab {
  private readonly trace: Trace;
  private readonly displayName: string;
  private readonly dataSource: SQLDataSource;
  private readonly schema: SchemaRegistry;
  private readonly rootSchema: string;
  private readonly initialFilters: readonly Filter[];
  private gridApi?: DataGridApi;
  private columns: readonly Column[] = [];

  constructor(config: {
    trace: Trace;
    displayName: string;
    dataSource: SQLDataSource;
    schema: SchemaRegistry;
    rootSchema: string;
    initialFilters?: Filter[];
    initialColumns?: string[];
  }) {
    this.trace = config.trace;
    this.displayName = config.displayName;
    this.dataSource = config.dataSource;
    this.schema = config.schema;
    this.rootSchema = config.rootSchema;
    this.columns =
      config.initialColumns?.map((colName) => ({
        field: colName,
      })) ??
      getDefaultVisibleFields(this.schema, this.rootSchema).map((col) => {
        return {field: col};
      });
    this.initialFilters = config.initialFilters ?? [];
  }

  getTitle(): string {
    return `Table: ${this.displayName}`;
  }

  render(): m.Children {
    const rowCount = this.gridApi?.getRowCount();
    const rowCountText =
      rowCount !== undefined ? `${rowCount.toLocaleString()} rows` : '';

    // Get the current query from the datasource
    const currentQuery = this.dataSource.getCurrentQuery();

    return m(
      DetailsShell,
      {
        title: 'Table',
        description: this.displayName,
        fillHeight: true,
        buttons: [
          rowCountText && m('span.pf-table-explorer__row-count', rowCountText),
          currentQuery &&
            m(
              PopupMenu,
              {
                trigger: m(Button, {
                  label: 'Add debug track',
                }),
                position: PopupPosition.Top,
              },
              m(AddDebugTrackMenu, {
                trace: this.trace,
                query: currentQuery,
                availableColumns: this.columns.map((col) => col.field),
              }),
            ),
          m(DataGridExportButton, {
            onExportData: (format) => this.gridApi!.exportData(format),
          }),
        ],
      },
      m(DataGrid, {
        schema: this.schema,
        rootSchema: this.rootSchema,
        data: this.dataSource,
        initialFilters: this.initialFilters,
        fillHeight: true,
        columns: this.columns,
        onReady: (api) => {
          this.gridApi = api;
        },
        onColumnsChanged: (columns) => {
          this.columns = columns;
        },
      } satisfies DataGridAttrs),
    );
  }
}
