import m from 'mithril';
import {Tab} from '../../public/tab';
import {
  DataGrid,
  DataGridApi,
  DataGridAttrs,
} from '../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {DetailsShell} from '../../widgets/details_shell';
import {Filter} from '../../components/widgets/datagrid/model';
import {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
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
  private readonly initialFilters?: Filter[];
  private readonly initialColumns?: string[];
  private gridApi?: DataGridApi;

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
    this.initialFilters = config.initialFilters;
    this.initialColumns = config.initialColumns;
  }

  getTitle(): string {
    return `Table: ${this.displayName}`;
  }

  render(): m.Children {
    const rowCount = this.gridApi?.getRowCount();
    const rowCountText =
      rowCount !== undefined ? `${rowCount.toLocaleString()} rows` : '';

    // Get available columns from schema for debug track menu
    const schemaColumns = this.schema[this.rootSchema];
    const availableColumns =
      schemaColumns !== undefined ? Object.keys(schemaColumns) : [];

    return m(
      DetailsShell,
      {
        title: 'Table',
        description: this.displayName,
        fillHeight: true,
        buttons: [
          rowCountText && m('span.pf-table-explorer__row-count', rowCountText),
          m(
            PopupMenu,
            {
              trigger: m(Button, {label: 'Add debug track'}),
              position: PopupPosition.Top,
            },
            m(AddDebugTrackMenu, {
              trace: this.trace,
              query: `SELECT * FROM ${this.displayName}`,
              availableColumns,
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
        initialColumns: this.initialColumns?.map((colName) => ({
          field: colName,
        })),
        fillHeight: true,
        onReady: (api) => {
          this.gridApi = api;
        },
      } satisfies DataGridAttrs),
    );
  }
}
