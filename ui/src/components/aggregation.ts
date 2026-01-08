// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
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
import {ColorScheme} from '../base/color_scheme';
import {Row, SqlValue} from '../trace_processor/query_result';
import {CellRenderResult} from './widgets/datagrid/datagrid_schema';

export interface ColumnDef {
  readonly columnId: string;
  readonly title: string;
  readonly formatHint?: string;
  readonly sum?: boolean;
  readonly sort?: 'ASC' | 'DESC';
  // Additional fields this column depends on for rendering.
  // These fields will be included in queries and made available in the row
  // parameter passed to cellRenderer, even if they're not visible columns.
  readonly dependsOn?: readonly string[];
  // Custom renderer for this column's cells. If provided, this takes precedence
  // over the formatHint-based renderer.
  readonly cellRenderer?: (
    value: SqlValue,
    row: Row,
  ) => m.Children | CellRenderResult;
}

export interface BarChartData {
  readonly title: string;
  readonly value: number;
  readonly color: ColorScheme;
}

export interface Sorting {
  readonly column: string;
  readonly direction: 'DESC' | 'ASC';
}
