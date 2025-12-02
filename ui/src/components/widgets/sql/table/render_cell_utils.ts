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

import m from 'mithril';
import {MenuItem} from '../../../../widgets/menu';
import {SqlValue} from '../../../../trace_processor/query_result';
import {isString} from '../../../../base/object_utils';
import {Icons} from '../../../../base/semantic_icons';
import {copyToClipboard} from '../../../../base/clipboard';
import {sqlValueToReadableString} from '../../../../trace_processor/sql_utils';
import {RenderedCell, TableManager} from './table_column';

function copyMenuItem(label: string, value: string): m.Child {
  return m(MenuItem, {
    icon: Icons.Copy,
    label,
    onclick: () => {
      copyToClipboard(value);
    },
  });
}

// Return a list of "standard" menu items for the given cell.
// Note: Filter items are not included as DataGrid handles filtering.
export function getStandardContextMenuItems(value: SqlValue): m.Child[] {
  const result: m.Child[] = [];

  if (isString(value)) {
    result.push(copyMenuItem('Copy', value));
  }

  return result;
}

export function displayValue(value: SqlValue): m.Child {
  if (value === null) {
    return 'null';
  }
  return sqlValueToReadableString(value);
}

export function renderStandardCell(
  value: SqlValue,
  tableManager: TableManager | undefined,
): RenderedCell {
  const contentWithFormatting = {
    content: displayValue(value),
    isNumerical: typeof value === 'number' || typeof value === 'bigint',
    isNull: value == null,
  };

  if (tableManager === undefined) {
    return contentWithFormatting;
  }
  const contextMenuItems: m.Child[] = getStandardContextMenuItems(value);
  return {
    ...contentWithFormatting,
    menu: contextMenuItems.length > 0 ? contextMenuItems : undefined,
  };
}
