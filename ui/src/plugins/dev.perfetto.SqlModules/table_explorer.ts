import m from 'mithril';
import {Tab} from '../../public/tab';
import {
  DataGrid,
  DataGridApi,
  DataGridAttrs,
} from '../../components/widgets/datagrid/datagrid';
import {SQLDataSource} from '../../components/widgets/datagrid/sql_data_source';
import {DetailsShell} from '../../widgets/details_shell';
import {Filter, Column, Pivot} from '../../components/widgets/datagrid/model';
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

export interface TableExplorerConfig {
  trace: Trace;
  displayName: string;
  dataSource: SQLDataSource;
  schema: SchemaRegistry;
  rootSchema: string;
  initialFilters?: Filter[];
  initialColumns?: Column[];
  initialPivot?: Pivot;
  onDuplicate?: (state: TableExplorerState) => void;
}

export interface TableExplorerState {
  filters: readonly Filter[];
  columns: readonly Column[];
  pivot?: Pivot;
}

export class TableExplorer implements Tab {
  private readonly trace: Trace;
  private readonly displayName: string;
  private readonly dataSource: SQLDataSource;
  private readonly schema: SchemaRegistry;
  private readonly rootSchema: string;
  private readonly onDuplicate?: (state: TableExplorerState) => void;
  private gridApi?: DataGridApi;
  private columns: readonly Column[] = [];
  private filters: readonly Filter[] = [];
  private pivot?: Pivot;

  constructor(config: TableExplorerConfig) {
    this.trace = config.trace;
    this.displayName = config.displayName;
    this.dataSource = config.dataSource;
    this.schema = config.schema;
    this.rootSchema = config.rootSchema;
    this.onDuplicate = config.onDuplicate;
    this.columns =
      config.initialColumns ??
      getDefaultVisibleFields(this.schema, this.rootSchema).map((col) => {
        return {field: col};
      });
    this.filters = config.initialFilters ?? [];
    this.pivot = config.initialPivot;
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
          this.onDuplicate &&
            m(Button, {
              label: 'Duplicate tab',
              icon: 'tab_duplicate',
              onclick: () => {
                this.onDuplicate?.({
                  filters: this.filters,
                  columns: this.columns,
                  pivot: this.pivot,
                });
              },
            }),
        ],
      },
      m(DataGrid, {
        schema: this.schema,
        rootSchema: this.rootSchema,
        data: this.dataSource,
        filters: this.filters,
        pivot: this.pivot,
        fillHeight: true,
        columns: this.columns,
        onReady: (api) => {
          this.gridApi = api;
        },
        onColumnsChanged: (columns) => {
          this.columns = columns;
        },
        onFiltersChanged: (filters) => {
          this.filters = filters;
        },
        onPivotChanged: (pivot) => {
          this.pivot = pivot;
        },
      } satisfies DataGridAttrs),
    );
  }
}
