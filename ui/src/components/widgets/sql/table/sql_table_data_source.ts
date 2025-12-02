// Copyright (C) 2024 The Android Open Source Project
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

import {
  DataGridDataSource,
  DataGridModel,
  DataSourceResult,
  RowDef,
} from '../../data_grid/common';
import {SqlTableState} from './state';

/**
 * Adapter that makes SqlTableState work as a DataGridDataSource.
 * This allows SqlTable to use DataGrid internally while maintaining
 * its own state management.
 */
export class SqlTableDataSource implements DataGridDataSource {
  constructor(private readonly state: SqlTableState) {}

  get rows(): DataSourceResult | undefined {
    const displayedRows = this.state.getDisplayedRows();
    const totalCount = this.state.getTotalRowCount();

    return {
      totalRows: totalCount ?? displayedRows.length,
      rowOffset: this.state.getCurrentOffset(),
      rows: displayedRows,
      aggregates: {}, // SqlTable doesn't use aggregates from DataGrid
      isLoading: this.state.isLoading(),
    };
  }

  get isLoading(): boolean {
    return this.state.isLoading();
  }

  notifyUpdate(model: DataGridModel): void {
    // Handle pagination updates from DataGrid
    if (model.pagination) {
      this.state.setPagination(model.pagination.offset, model.pagination.limit);
    }
  }

  async exportData(): Promise<readonly RowDef[]> {
    throw new Error('Exporting data is not supported for SqlTableDataSource.');
  }
}
