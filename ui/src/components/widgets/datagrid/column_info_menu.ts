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

import m from 'mithril';
import {MenuItem} from '../../../widgets/menu';
import {ColumnInfo, isParameterizedColumnDef} from './datagrid_schema';

interface ColumnInfoMenuAttrs {
  readonly field: string;
  readonly colInfo: ColumnInfo | undefined;
  readonly aggregateFunc?: string;
}

/**
 * Renders a "Column Info" submenu showing details about a column.
 * Shows field path, column type, and other schema information.
 */
export class ColumnInfoMenu implements m.ClassComponent<ColumnInfoMenuAttrs> {
  view({attrs}: m.Vnode<ColumnInfoMenuAttrs>): m.Children {
    const {field, colInfo, aggregateFunc} = attrs;

    const infoItems: m.Children[] = [];

    // Field path (always shown)
    infoItems.push(
      m(MenuItem, {
        label: `Field: ${field}`,
        disabled: true,
      }),
    );

    // Aggregate function (if applicable)
    if (aggregateFunc) {
      infoItems.push(
        m(MenuItem, {
          label: `Aggregate: ${aggregateFunc}`,
          disabled: true,
        }),
      );
    }

    if (colInfo) {
      // Column type
      if (colInfo.columnType) {
        infoItems.push(
          m(MenuItem, {
            label: `Type: ${colInfo.columnType}`,
            disabled: true,
          }),
        );
      }

      // Parameterized column info
      if (isParameterizedColumnDef(colInfo.def)) {
        infoItems.push(
          m(MenuItem, {
            label: 'Parameterized: yes',
            disabled: true,
          }),
        );
        if (colInfo.paramKey) {
          infoItems.push(
            m(MenuItem, {
              label: `Key: ${colInfo.paramKey}`,
              disabled: true,
            }),
          );
        }
      }

      // Custom renderer indicator
      if (colInfo.cellRenderer) {
        infoItems.push(
          m(MenuItem, {
            label: 'Custom renderer: yes',
            disabled: true,
          }),
        );
      }

      // Custom formatter indicator
      if (colInfo.cellFormatter) {
        infoItems.push(
          m(MenuItem, {
            label: 'Custom formatter: yes',
            disabled: true,
          }),
        );
      }
    } else {
      infoItems.push(
        m(MenuItem, {
          label: 'No schema info',
          disabled: true,
        }),
      );
    }

    return m(MenuItem, {label: 'Column info', icon: 'info'}, infoItems);
  }
}
