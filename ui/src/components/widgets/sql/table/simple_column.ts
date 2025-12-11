// Copyright (C) 2025 The Android Open Source Project
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

import {SqlValue} from '../../../../trace_processor/query_result';
import {PerfettoSqlType} from '../../../../trace_processor/perfetto_sql_type';
import {renderStandardCell} from './render_cell_utils';
import {SqlColumn} from './sql_column';
import {TableColumn, TableManager} from './table_column';

export class SimpleColumn implements TableColumn {
  public readonly type: PerfettoSqlType | undefined = undefined;

  constructor(public readonly column: SqlColumn) {}

  primaryColumn(): SqlColumn {
    return this.column;
  }

  renderCell(value: SqlValue, tableManager: TableManager | undefined) {
    return renderStandardCell(value, this.column, tableManager);
  }
}
