// Copyright (C) 2021 The Android Open Source Project
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

import {Actions} from '../common/actions';
import {
  AggregationAttrs,
  isStackPivot,
  PivotAttrs,
  TableAttrs
} from '../common/pivot_table_common';
import {globals} from './globals';

export function isAggregationAttrs(attrs: PivotAttrs|AggregationAttrs):
    attrs is AggregationAttrs {
  return (attrs as AggregationAttrs).aggregation !== undefined;
}

function equalTableAttrs(
    left: PivotAttrs|AggregationAttrs, right: PivotAttrs|AggregationAttrs) {
  if (left.columnName !== right.columnName) {
    return false;
  }

  if (left.tableName !== right.tableName) {
    return false;
  }

  if (isAggregationAttrs(left) && isAggregationAttrs(right)) {
    if (left.aggregation !== right.aggregation) {
      return false;
    }
  }
  return true;
}

export function getDataTransferType(isPivot: boolean) {
  if (isPivot) {
    return 'perfetto/pivot-table-dragged-pivot';
  }
  return 'perfetto/pivot-table-dragged-aggregation';
}

export class PivotTableHelper {
  readonly pivotTableId: string;
  readonly availableColumns: TableAttrs[];
  readonly availableAggregations: string[];
  readonly totalColumnsCount = 0;

  private _selectedPivots: PivotAttrs[] = [];
  private _selectedAggregations: AggregationAttrs[] = [];
  private _isPivot = true;
  private _selectedColumnIndex = 0;
  private _selectedAggregationIndex = 0;
  private _editPivotTableModalOpen = false;

  constructor(
      pivotTableId: string, availableColumns: TableAttrs[],
      availableAggregations: string[], selectedPivots: PivotAttrs[],
      selectedAggregations: AggregationAttrs[]) {
    this.pivotTableId = pivotTableId;
    this.availableColumns = availableColumns;
    for (const table of this.availableColumns) {
      this.totalColumnsCount += table.columns.length;
    }
    this.availableAggregations = availableAggregations;
    this.setSelectedPivotsAndAggregations(selectedPivots, selectedAggregations);
  }

  // Sets selected pivots and aggregations if the editor modal is not open.
  setSelectedPivotsAndAggregations(
      selectedPivots: PivotAttrs[], selectedAggregations: AggregationAttrs[]) {
    if (!this.editPivotTableModalOpen) {
      // Making a copy of selectedPivots and selectedAggregations to preserve
      // the original state.
      this._selectedPivots =
          selectedPivots.map(pivot => Object.assign({}, pivot));
      this._selectedAggregations = selectedAggregations.map(
          aggregation => Object.assign({}, aggregation));
    }
  }

  // Dictates if the selected indexes refer to a pivot or aggregation.
  togglePivotSelection() {
    this._isPivot = !this._isPivot;
    if (this._isPivot === false) {
      const selectedColumn = this.getSelectedPivotTableColumnAttrs();
      if (isStackPivot(selectedColumn.tableName, selectedColumn.columnName)) {
        this._selectedColumnIndex = Math.max(0, this._selectedColumnIndex - 1);
      }
    }
  }

  setSelectedPivotTableColumnIndex(index: number) {
    if (index < 0 && index >= this.totalColumnsCount) {
      throw Error(`Selected column index "${index}" out of bounds.`);
    }
    this._selectedColumnIndex = index;
  }

  setSelectedPivotTableAggregationIndex(index: number) {
    if (index < 0 && index >= this.availableAggregations.length) {
      throw Error(`Selected aggregation index "${index}" out of bounds.`);
    }
    this._selectedAggregationIndex = index;
  }

  // Get column attributes on selectedColumnIndex and
  // selectedAggregationIndex.
  getSelectedPivotTableColumnAttrs(): PivotAttrs|AggregationAttrs {
    let tableName, columnName;
    // Finds column index relative to its table.
    let colIdx = this._selectedColumnIndex;
    for (const {tableName: tblName, columns} of this.availableColumns) {
      if (colIdx < columns.length) {
        tableName = tblName;
        columnName = columns[colIdx];
        break;
      }
      colIdx -= columns.length;
    }
    if (tableName === undefined || columnName === undefined) {
      throw Error(
          'Pivot table selected column does not exist in availableColumns.');
    }

    // Get aggregation if selected column is not a pivot, undefined otherwise.
    if (!this._isPivot) {
      const aggregation =
          this.availableAggregations[this._selectedAggregationIndex];
      return {tableName, columnName, aggregation, order: 'DESC'};
    }

    return {
      tableName,
      columnName,
      isStackPivot: isStackPivot(tableName, columnName)
    };
  }

  // Adds column based on selected index to selectedPivots or
  // selectedAggregations if it doesn't already exist, remove otherwise.
  updatePivotTableColumnOnSelectedIndex() {
    const columnAttrs = this.getSelectedPivotTableColumnAttrs();
    this.updatePivotTableColumnOnColumnAttributes(columnAttrs);
  }

  // Adds column based on column attributes to selectedPivots or
  // selectedAggregations if it doesn't already exist, remove otherwise.
  updatePivotTableColumnOnColumnAttributes(columnAttrs: PivotAttrs|
                                           AggregationAttrs) {
    let storage: Array<PivotAttrs|AggregationAttrs>;
    let attrs: PivotAttrs|AggregationAttrs;
    if (isAggregationAttrs(columnAttrs)) {
      if (isStackPivot(columnAttrs.tableName, columnAttrs.columnName)) {
        throw Error(
            `Stack column "${columnAttrs.tableName} ${
                columnAttrs.columnName}" should not ` +
            `be added as an aggregation.`);
      }
      storage = this._selectedAggregations;
      attrs = {
        tableName: columnAttrs.tableName,
        columnName: columnAttrs.columnName,
        aggregation: columnAttrs.aggregation,
        order: columnAttrs.order
      };
    } else {
      storage = this._selectedPivots;
      attrs = {
        tableName: columnAttrs.tableName,
        columnName: columnAttrs.columnName,
        isStackPivot: columnAttrs.isStackPivot
      };
    }
    const index =
        storage.findIndex(element => equalTableAttrs(element, columnAttrs));

    if (index === -1) {
      storage.push(attrs);
    } else {
      storage.splice(index, 1);
    }
  }

  clearPivotTableColumns() {
    this._selectedPivots = [];
    this._selectedAggregations = [];
  }

  // Changes aggregation sorting from DESC to ASC and vice versa.
  togglePivotTableAggregationSorting(index: number) {
    if (index < 0 || index >= this._selectedAggregations.length) {
      throw Error(`Column index "${index}" is out of bounds.`);
    }
    this._selectedAggregations[index].order =
        this._selectedAggregations[index].order === 'DESC' ? 'ASC' : 'DESC';
  }

  // Moves target column to the requested destination.
  reorderPivotTableDraggedColumn(
      isPivot: boolean, targetColumnIdx: number, destinationColumnIdx: number) {
    let storage: Array<PivotAttrs|AggregationAttrs>;
    if (isPivot) {
      storage = this._selectedPivots;
    } else {
      storage = this._selectedAggregations;
    }

    if (targetColumnIdx < 0 || targetColumnIdx >= storage.length) {
      throw Error(`Target column index "${targetColumnIdx}" out of bounds.`);
    }
    if (destinationColumnIdx < 0 || destinationColumnIdx >= storage.length) {
      throw Error(
          `Destination column index "${destinationColumnIdx}" out of bounds.`);
    }

    const targetColumn: PivotAttrs|AggregationAttrs = storage[targetColumnIdx];
    storage.splice(targetColumnIdx, 1);
    storage.splice(destinationColumnIdx, 0, targetColumn);
  }

  selectedColumnOnDrag(e: DragEvent, isPivot: boolean, targetIdx: number) {
    const dataTransferType = getDataTransferType(isPivot);
    if (e.dataTransfer === null) {
      return;
    }
    e.dataTransfer.setData(dataTransferType, targetIdx.toString());
  }

  selectedColumnOnDrop(
      e: DragEvent, isPivot: boolean, destinationColumnIdx: number) {
    const dataTransferType = getDataTransferType(isPivot);
    if (e.dataTransfer === null) {
      return;
    }
    // Prevents dragging pivots to aggregations and vice versa.
    if (!e.dataTransfer.types.includes(dataTransferType)) {
      return;
    }

    const targetColumnIdxString = e.dataTransfer.getData(dataTransferType);
    const targetColumnIdx = Number(targetColumnIdxString);
    if (!Number.isInteger(targetColumnIdx)) {
      throw Error(
          `Target column index "${targetColumnIdxString}" is not valid.`);
    }

    this.reorderPivotTableDraggedColumn(
        isPivot, targetColumnIdx, destinationColumnIdx);
    e.dataTransfer.clearData(dataTransferType);
  }


  // Highlights valid drop locations when dragging over them.
  highlightDropLocation(e: DragEvent, isPivot: boolean) {
    if (e.dataTransfer === null) {
      return;
    }
    // Prevents highlighting aggregations when dragging pivots over them
    // and vice versa.
    if (!e.dataTransfer.types.includes(getDataTransferType(isPivot))) {
      return;
    }
    (e.target as HTMLTableDataCellElement).classList.add('drop-location');
  }

  removeHighlightFromDropLocation(e: DragEvent) {
    (e.target as HTMLTableDataCellElement).classList.remove('drop-location');
  }

  // Gets column index in availableColumns based on its attributes.
  getColumnIndex(columnAttrs: PivotAttrs|AggregationAttrs) {
    let index = 0;
    for (const {tableName, columns} of this.availableColumns) {
      if (tableName === columnAttrs.tableName) {
        const colIdx =
            columns.findIndex(column => column === columnAttrs.columnName);
        return colIdx === -1 ? -1 : index + colIdx;
      }
      index += columns.length;
    }
    return -1;
  }

  selectPivotTableColumn(columnAttrs: PivotAttrs|AggregationAttrs) {
    this._isPivot = !isAggregationAttrs(columnAttrs);

    const colIndex = this.getColumnIndex(columnAttrs);
    if (colIndex === -1) {
      throw Error(`Selected column "${columnAttrs.tableName} ${
          columnAttrs.columnName}" not found in availableColumns.`);
    }
    this.setSelectedPivotTableColumnIndex(colIndex);

    if (isAggregationAttrs(columnAttrs)) {
      const aggIndex = this.availableAggregations.findIndex(
          aggregation => aggregation === columnAttrs.aggregation);
      if (aggIndex === -1) {
        throw Error(`Selected aggregation "${
            columnAttrs.aggregation}" not found in availableAggregations.`);
      }
      this.setSelectedPivotTableAggregationIndex(aggIndex);
    }
  }

  queryPivotTableChanges() {
    globals.dispatch(Actions.setSelectedPivotsAndAggregations({
      pivotTableId: this.pivotTableId,
      selectedPivots: this._selectedPivots,
      selectedAggregations: this._selectedAggregations
    }));
    globals.dispatch(Actions.setPivotTableRequest(
        {pivotTableId: this.pivotTableId, action: 'QUERY'}));
  }

  toggleEditPivotTableModal() {
    this._editPivotTableModalOpen = !this._editPivotTableModalOpen;
  }

  get selectedPivots() {
    return this._selectedPivots.map(pivot => Object.assign({}, pivot));
  }

  get selectedAggregations() {
    return this._selectedAggregations.map(
        aggregation => Object.assign({}, aggregation));
  }

  get isPivot() {
    return this._isPivot;
  }

  get selectedColumnIndex() {
    return this._selectedColumnIndex;
  }

  get selectedAggregationIndex() {
    return this._selectedAggregationIndex;
  }

  get editPivotTableModalOpen() {
    return this._editPivotTableModalOpen;
  }
}
