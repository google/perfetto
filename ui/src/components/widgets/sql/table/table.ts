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
import {LegacyTableManager} from '../legacy_table/column';
import {SqlValue} from '../../../../trace_processor/query_result';
import {Timestamp} from '../../timestamp';
import {Duration, Time} from '../../../../base/time';
import {
  renderStandardCell,
  getStandardContextMenuItems,
} from '../legacy_table/render_cell_utils';
import {DurationWidget} from '../../duration';
import {SliceRef} from '../../slice';
import {
  asSchedSqlId,
  asSliceSqlId,
  asThreadStateSqlId,
  asUpid,
  asUtid,
} from '../../../sql_utils/core_types';
import {SchedRef} from '../../sched';
import {ThreadStateRef} from '../../thread_state';
import {PopupMenu2} from '../../../../widgets/menu';
import {Anchor} from '../../../../widgets/anchor';
import {showThreadDetailsMenuItem} from '../../thread';
import {showProcessDetailsMenuItem} from '../../process';

export interface SimpleColumn {
  name: string;
  renderCell: (value: SqlValue, tableManager: LegacyTableManager) => m.Children;
}

function renderNumericCell(
  name: string,
  value: SqlValue,
  tableManager: LegacyTableManager,
  renderBigint: (value: bigint) => m.Children,
): m.Children {
  if (value === null || typeof value !== 'bigint') {
    return renderStandardCell(value, name, tableManager);
  }
  return renderBigint(value);
}

export function createStandardColumn(name: string): SimpleColumn {
  return {
    name: name,
    renderCell: (value: SqlValue, tableManager: LegacyTableManager) =>
      renderStandardCell(value, name, tableManager),
  };
}

export function createTimestampColumn(name: string): SimpleColumn {
  const col = createStandardColumn(name);
  col.renderCell = (value: SqlValue, tableManager: LegacyTableManager) =>
    renderNumericCell(name, value, tableManager, (value: bigint) => {
      return m(Timestamp, {
        ts: Time.fromRaw(value),
        extraMenuItems: getStandardContextMenuItems(value, name, tableManager),
      });
    });
  return col;
}

export function createDurationColumn(name: string): SimpleColumn {
  const col = createStandardColumn(name);
  col.renderCell = (value: SqlValue, tableManager: LegacyTableManager) =>
    renderNumericCell(name, value, tableManager, (value: bigint) => {
      return m(DurationWidget, {
        dur: Duration.fromRaw(value),
        extraMenuItems: getStandardContextMenuItems(value, name, tableManager),
      });
    });
  return col;
}

export function createSliceIdColumn(name: string): SimpleColumn {
  const col = createStandardColumn(name);
  col.renderCell = (value: SqlValue, tableManager: LegacyTableManager) =>
    renderNumericCell(name, value, tableManager, (value: bigint) => {
      return m(SliceRef, {
        id: asSliceSqlId(Number(value)),
        name: `${value}`,
        switchToCurrentSelectionTab: false,
      });
    });
  return col;
}

export function createThreadIdColumn(name: string): SimpleColumn {
  const col = createStandardColumn(name);
  col.renderCell = (value: SqlValue, tableManager: LegacyTableManager) =>
    renderNumericCell(name, value, tableManager, (utid: bigint) => {
      return m(
        PopupMenu2,
        {
          trigger: m(Anchor, `${utid}`),
        },
        showThreadDetailsMenuItem(asUtid(Number(utid))),
        getStandardContextMenuItems(utid, name, tableManager),
      );
    });
  return col;
}

export function createProcessIdColumn(name: string): SimpleColumn {
  const col = createStandardColumn(name);
  col.renderCell = (value: SqlValue, tableManager: LegacyTableManager) =>
    renderNumericCell(name, value, tableManager, (upid: bigint) => {
      return m(
        PopupMenu2,
        {
          trigger: m(Anchor, `${upid}`),
        },
        showProcessDetailsMenuItem(asUpid(Number(upid))),
        getStandardContextMenuItems(upid, name, tableManager),
      );
    });
  return col;
}

export function createSchedIdColumn(name: string): SimpleColumn {
  const col = createStandardColumn(name);
  col.renderCell = (value: SqlValue, tableManager: LegacyTableManager) =>
    renderNumericCell(name, value, tableManager, (value: bigint) => {
      return m(SchedRef, {
        id: asSchedSqlId(Number(value)),
        name: `${value}`,
        switchToCurrentSelectionTab: false,
      });
    });
  return col;
}

export function createThreadStateIdColumn(name: string): SimpleColumn {
  const col = createStandardColumn(name);
  col.renderCell = (value: SqlValue, tableManager: LegacyTableManager) =>
    renderNumericCell(name, value, tableManager, (value: bigint) => {
      return m(ThreadStateRef, {
        id: asThreadStateSqlId(Number(value)),
        name: `${value}`,
        switchToCurrentSelectionTab: false,
      });
    });
  return col;
}
