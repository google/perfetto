// Copyright (C) 2023 The Android Open Source Project
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

import {copyToClipboard} from '../../base/clipboard';
import {isString} from '../../base/object_utils';
import {Icons} from '../../base/semantic_icons';
import {sqliteString} from '../../base/string_utils';
import {duration, Duration, Time} from '../../base/time';
import {Row, SqlValue} from '../../common/query_result';
import {Anchor} from '../../widgets/anchor';
import {Err} from '../../widgets/error';
import {MenuItem, PopupMenu2} from '../../widgets/menu';
import {SliceRef} from '../sql/slice';
import {asSliceSqlId} from '../sql_types';
import {sqlValueToString} from '../sql_utils';
import {Timestamp} from '../widgets/timestamp';

import {Column} from './column';
import {SqlTableState} from './state';
import {SliceIdDisplayConfig} from './table_description';

// This file is responsible for rendering a value in a given sell based on the
// column type.

function filterOptionMenuItem(
    label: string, filter: string, state: SqlTableState): m.Child {
  return m(MenuItem, {
    label,
    onclick: () => {
      state.addFilter(filter);
    },
  });
}

function getStandardFilters(
    c: Column, value: SqlValue, state: SqlTableState): m.Child[] {
  if (value === null) {
    return [
      filterOptionMenuItem('is null', `${c.expression} is null`, state),
      filterOptionMenuItem('is not null', `${c.expression} is not null`, state),
    ];
  }
  if (isString(value)) {
    return [
      filterOptionMenuItem(
          'equals to', `${c.expression} = ${sqliteString(value)}`, state),
      filterOptionMenuItem(
          'not equals to', `${c.expression} != ${sqliteString(value)}`, state),
    ];
  }
  if (typeof value === 'bigint' || typeof value === 'number') {
    return [
      filterOptionMenuItem('equals to', `${c.expression} = ${value}`, state),
      filterOptionMenuItem(
          'not equals to', `${c.expression} != ${value}`, state),
      filterOptionMenuItem('greater than', `${c.expression} > ${value}`, state),
      filterOptionMenuItem(
          'greater or equals than', `${c.expression} >= ${value}`, state),
      filterOptionMenuItem('less than', `${c.expression} < ${value}`, state),
      filterOptionMenuItem(
          'less or equals than', `${c.expression} <= ${value}`, state),
    ];
  }
  return [];
}

function displayValue(value: SqlValue): m.Child {
  if (value === null) {
    return m('i', 'NULL');
  }
  return sqlValueToString(value);
}

function displayDuration(value: duration): string;
function displayDuration(value: SqlValue): m.Children;
function displayDuration(value: SqlValue): m.Children {
  if (typeof value !== 'bigint') return displayValue(value);
  return Duration.format(value);
}

function display(column: Column, row: Row): m.Children {
  const value = row[column.alias];

  // Handle all cases when we have non-trivial formatting.
  switch (column.display?.type) {
    case 'duration':
    case 'thread_duration':
      return displayDuration(value);
  }

  return displayValue(value);
}

function copyMenuItem(label: string, value: string): m.Child {
  return m(MenuItem, {
    icon: Icons.Copy,
    label,
    onclick: () => {
      copyToClipboard(value);
    },
  });
}

function getContextMenuItems(
    column: Column, row: Row, state: SqlTableState): m.Child[] {
  const result: m.Child[] = [];
  const value = row[column.alias];

  if ((column.display?.type === 'duration' ||
       column.display?.type === 'thread_duration') &&
      typeof value === 'bigint') {
    result.push(copyMenuItem('Copy raw duration', `${value}`));
    result.push(
        copyMenuItem('Copy formatted duration', displayDuration(value)));
  }
  if (isString(value)) {
    result.push(copyMenuItem('Copy', value));
  }

  const filters = getStandardFilters(column, value, state);
  if (filters.length > 0) {
    result.push(
        m(MenuItem, {label: 'Add filter', icon: Icons.Filter}, ...filters));
  }

  return result;
}

function renderStandardColumn(
    column: Column, row: Row, state: SqlTableState): m.Children {
  const displayValue = display(column, row);
  const contextMenuItems: m.Child[] = getContextMenuItems(column, row, state);
  return m(
      PopupMenu2,
      {
        trigger: m(Anchor, displayValue),
      },
      ...contextMenuItems,
  );
}

function renderTimestampColumn(
    column: Column, row: Row, state: SqlTableState): m.Children {
  const value = row[column.alias];
  if (typeof value !== 'bigint') {
    return renderStandardColumn(column, row, state);
  }

  return m(Timestamp, {
    ts: Time.fromRaw(value),
    extraMenuItems: getContextMenuItems(column, row, state),
  });
}

function renderSliceIdColumn(
    column: {alias: string, display: SliceIdDisplayConfig},
    row: Row): m.Children {
  const config = column.display;
  const id = row[column.alias];
  const ts = row[config.ts];
  const dur = row[config.dur] === null ? -1n : row[config.dur];
  const trackId = row[config.trackId];

  const columnNotFoundError = (type: string, name: string) =>
      m(Err, `${type} column ${name} not found`);
  const wrongTypeError = (type: string, name: string, value: SqlValue) =>
      m(Err,
        `Wrong type for ${type} column ${name}: bigint expected, ${
            typeof value} found`);

  if (typeof id !== 'bigint') return sqlValueToString(id);
  if (ts === undefined) return columnNotFoundError('Timestamp', config.ts);
  if (typeof ts !== 'bigint') return wrongTypeError('timestamp', config.ts, ts);
  if (dur === undefined) return columnNotFoundError('Duration', config.dur);
  if (typeof dur !== 'bigint') {
    return wrongTypeError('duration', config.dur, ts);
  }
  if (trackId === undefined) return columnNotFoundError('Track id', trackId);
  if (typeof trackId !== 'bigint') {
    return wrongTypeError('track id', config.trackId, trackId);
  }

  return m(SliceRef, {
    id: asSliceSqlId(Number(id)),
    name: `${id}`,
    ts: Time.fromRaw(ts),
    dur: dur,
    sqlTrackId: Number(trackId),
    switchToCurrentSelectionTab: false,
  });
}

export function renderCell(
    column: Column, row: Row, state: SqlTableState): m.Children {
  if (column.display && column.display.type === 'slice_id') {
    return renderSliceIdColumn(
        {alias: column.alias, display: column.display}, row);
  } else if (column.display && column.display.type === 'timestamp') {
    return renderTimestampColumn(column, row, state);
  }
  return renderStandardColumn(column, row, state);
}
