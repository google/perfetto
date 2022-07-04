/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import * as m from 'mithril';

import {sqliteString} from '../base/string_utils';
import {Actions, DeferredAction} from '../common/actions';
import {COUNT_AGGREGATION} from '../common/empty_state';
import {ColumnType} from '../common/query_result';
import {
  Area,
  PivotTableReduxAreaState,
  PivotTableReduxQuery,
  PivotTableReduxResult,
  SortDirection,
} from '../common/state';
import {fromNs, timeToCode} from '../common/time';
import {
  PivotTableReduxController,
  PivotTree,
} from '../controller/pivot_table_redux_controller';

import {globals} from './globals';
import {fullscreenModalContainer, ModalDefinition} from './modal';
import {Panel} from './panel';
import {AnyAttrsVnode} from './panel_container';
import {ArgumentPopup} from './pivot_table_redux_argument_popup';
import {
  Aggregation,
  AggregationFunction,
  aggregationIndex,
  areaFilter,
  expression,
  generateQuery,
  QueryGeneratorError,
  sliceAggregationColumns,
  Table,
  TableColumn,
  tableColumnEquals,
  tables,
  threadSliceAggregationColumns,
} from './pivot_table_redux_query_generator';
import {PopupMenuButton, PopupMenuItem} from './popup_menu';

interface PathItem {
  tree: PivotTree;
  nextKey: ColumnType;
}

// Used to convert TableColumn to a string in order to store it in a Map, as
// ES6 does not support compound Set/Map keys. This function should only be used
// for interning keys, and does not have any requirements beyond different
// TableColumn objects mapping to different strings.
export function columnKey(tableColumn: TableColumn): string {
  switch (tableColumn.kind) {
    case 'argument': {
      return `argument:${tableColumn.argument}`;
    }
    case 'regular': {
      return `${tableColumn.table}.${tableColumn.column}`;
    }
    default: {
      throw new Error(`malformed table column ${tableColumn}`);
    }
  }
}

export function aggregationKey(aggregation: Aggregation): string {
  return `${aggregation.aggregationFunction}:${columnKey(aggregation.column)}`;
}

// Arguments to an action to toggle a table column in a particular part of
// application's state.
interface ColumnSetArgs<T> {
  column: T;
  selected: boolean;
}

interface ColumnSetCheckboxAttrs<T> {
  set: (args: ColumnSetArgs<T>) => DeferredAction<ColumnSetArgs<T>>;
  get: Map<string, T>;
  setKey: T;
}

abstract class GenericCheckbox<T> implements
    m.ClassComponent<ColumnSetCheckboxAttrs<T>> {
  abstract keyFn(value: T): string;

  view({attrs}: m.Vnode<ColumnSetCheckboxAttrs<T>>) {
    return m('input[type=checkbox]', {
      onclick: (e: InputEvent) => {
        const target = e.target as HTMLInputElement;

        globals.dispatch(
            attrs.set({column: attrs.setKey, selected: target.checked}));
        globals.rafScheduler.scheduleFullRedraw();
      },
      checked: attrs.get.has(this.keyFn(attrs.setKey)),
    });
  }
}

class ColumnSetCheckbox extends GenericCheckbox<TableColumn> {
  keyFn(value: TableColumn): string {
    return columnKey(value);
  }
}

class AggregationCheckbox extends GenericCheckbox<Aggregation> {
  keyFn(value: Aggregation): string {
    return aggregationKey(value);
  }
}

interface PivotTableReduxAttrs {
  selectionArea: PivotTableReduxAreaState;
}

interface DrillFilter {
  column: TableColumn;
  value: ColumnType;
}

// Convert DrillFilter to SQL condition to be used in WHERE clause.
function renderDrillFilter(filter: DrillFilter): string {
  // TODO(b/231429468): This would not work for non-slice column.
  const column = expression(filter.column);
  if (filter.value === null) {
    return `${column} IS NULL`;
  } else if (typeof filter.value === 'number') {
    return `${column} = ${filter.value}`;
  }
  return `${column} = ${sqliteString(filter.value)}`;
}

function readableColumnName(column: TableColumn) {
  switch (column.kind) {
    case 'argument': {
      return `Argument ${column.argument}`;
    }
    case 'regular': {
      return `${column.table}.${column.column}`;
    }
    default: {
      throw new Error(`malformed table column ${column}`);
    }
  }
}

export class PivotTableRedux extends Panel<PivotTableReduxAttrs> {
  get selectedPivotsMap() {
    return globals.state.nonSerializableState.pivotTableRedux.selectedPivotsMap;
  }

  get selectedAggregations() {
    return globals.state.nonSerializableState.pivotTableRedux
        .selectedAggregations;
  }

  get constrainToArea() {
    return globals.state.nonSerializableState.pivotTableRedux.constrainToArea;
  }

  renderCanvas(): void {}

  generateQuery(attrs: PivotTableReduxAttrs): PivotTableReduxQuery {
    return generateQuery(
        this.selectedPivotsMap,
        this.selectedAggregations,
        globals.state.areas[attrs.selectionArea.areaId],
        this.constrainToArea);
  }

  renderTablePivotColumns(t: Table) {
    return m(
        'li',
        t.name,
        m('ul',
          t.columns.map(
              (col) =>
                  m('li',
                    m(ColumnSetCheckbox, {
                      get: this.selectedPivotsMap,
                      set: Actions.setPivotTablePivotSelected,
                      setKey: {kind: 'regular', table: t.name, column: col},
                    }),
                    col))));
  }

  renderResultsView(attrs: PivotTableReduxAttrs) {
    return m(
        '.pivot-table-redux',
        m('button.mode-button',
          {
            onclick: () => {
              globals.dispatch(Actions.setPivotTableEditMode({editMode: true}));
              globals.rafScheduler.scheduleFullRedraw();
            },
          },
          'Edit'),
        this.renderResultsTable(attrs));
  }

  renderDrillDownCell(
      area: Area, result: PivotTableReduxResult, filters: DrillFilter[]) {
    return m(
        'td',
        m('button',
          {
            title: 'All corresponding slices',
            onclick: () => {
              const queryFilters = filters.map(renderDrillFilter);
              if (this.constrainToArea) {
                queryFilters.push(areaFilter(area));
              }
              const query = `
                select * from ${result.metadata.tableName}
                where ${queryFilters.join(' and \n')}
              `;
              // TODO(ddrone): the UI of running query as if it was a canned or
              // custom query is a temporary one, replace with a proper UI.
              globals.dispatch(Actions.executeQuery({
                queryId: `pivot_table_details_${
                    PivotTableReduxController.detailsCount++}`,
                query,
              }));
            },
          },
          m('i.material-icons', 'arrow_right')));
  }

  renderSectionRow(
      area: Area, path: PathItem[], tree: PivotTree,
      result: PivotTableReduxResult): m.Vnode {
    const renderedCells = [];
    for (let j = 0; j + 1 < path.length; j++) {
      renderedCells.push(m('td', m('span.indent', ' '), `${path[j].nextKey}`));
    }

    const treeDepth = result.metadata.pivotColumns.length;
    const colspan = treeDepth - path.length + 1;
    const button =
        m('button',
          {
            onclick: () => {
              tree.isCollapsed = !tree.isCollapsed;
              globals.rafScheduler.scheduleFullRedraw();
            },
          },
          m('i.material-icons',
            tree.isCollapsed ? 'expand_more' : 'expand_less'));

    renderedCells.push(
        m('td', {colspan}, button, `${path[path.length - 1].nextKey}`));

    for (let i = 0; i < tree.aggregates.length; i++) {
      const renderedValue = this.renderCell(
          result.metadata.aggregationColumns[i].column, tree.aggregates[i]);
      renderedCells.push(m('td', renderedValue));
    }

    const drillFilters: DrillFilter[] = [];
    for (let i = 0; i < path.length; i++) {
      drillFilters.push({
        value: `${path[i].nextKey}`,
        column: result.metadata.pivotColumns[i],
      });
    }

    renderedCells.push(this.renderDrillDownCell(area, result, drillFilters));
    return m('tr', renderedCells);
  }

  renderCell(column: TableColumn, value: ColumnType): string {
    if (column.kind === 'regular' &&
        (column.column === 'dur' || column.column === 'thread_dur')) {
      if (typeof value === 'number') {
        return timeToCode(fromNs(value));
      }
    }
    return `${value}`;
  }

  renderTree(
      area: Area, path: PathItem[], tree: PivotTree,
      result: PivotTableReduxResult, sink: m.Vnode[]) {
    if (tree.isCollapsed) {
      sink.push(this.renderSectionRow(area, path, tree, result));
      return;
    }
    if (tree.children.size > 0) {
      // Avoid rendering the intermediate results row for the root of tree
      // and in case there's only one child subtree.
      if (!tree.isCollapsed && path.length > 0 && tree.children.size !== 1) {
        sink.push(this.renderSectionRow(area, path, tree, result));
      }
      for (const [key, childTree] of tree.children.entries()) {
        path.push({tree: childTree, nextKey: key});
        this.renderTree(area, path, childTree, result, sink);
        path.pop();
      }
      return;
    }

    // Avoid rendering the intermediate results row if it has only one leaf
    // row.
    if (!tree.isCollapsed && path.length > 0 && tree.rows.length > 1) {
      sink.push(this.renderSectionRow(area, path, tree, result));
    }
    for (const row of tree.rows) {
      const renderedCells = [];
      const drillFilters: DrillFilter[] = [];
      const treeDepth = result.metadata.pivotColumns.length;
      for (let j = 0; j < treeDepth; j++) {
        const value = this.renderCell(result.metadata.pivotColumns[j], row[j]);
        if (j < path.length) {
          renderedCells.push(m('td', m('span.indent', ' '), value));
        } else {
          renderedCells.push(m(`td`, value));
        }
        drillFilters.push(
            {column: result.metadata.pivotColumns[j], value: row[j]});
      }
      for (let j = 0; j < result.metadata.aggregationColumns.length; j++) {
        const value = row[aggregationIndex(treeDepth, j, treeDepth)];
        const renderedValue = this.renderCell(
            result.metadata.aggregationColumns[j].column, value);
        renderedCells.push(m('td', renderedValue));
      }

      renderedCells.push(this.renderDrillDownCell(area, result, drillFilters));
      sink.push(m('tr', renderedCells));
    }
  }

  renderTotalsRow(queryResult: PivotTableReduxResult) {
    const overallValuesRow =
        [m('td.total-values',
           {'colspan': queryResult.metadata.pivotColumns.length},
           m('strong', 'Total values:'))];
    for (let i = 0; i < queryResult.tree.aggregates.length; i++) {
      overallValuesRow.push(
          m('td',
            this.renderCell(
                queryResult.metadata.aggregationColumns[i].column,
                queryResult.tree.aggregates[i])));
    }
    overallValuesRow.push(m('td'));
    return m('tr', overallValuesRow);
  }

  sortingItem(column: TableColumn, order: SortDirection): PopupMenuItem {
    // Arrow contains unicode character for up or down arrow, according to the
    // direction of sorting.
    const arrow = order === 'DESC' ? '\u25BC' : '\u25B2';
    return {
      text: `Sort ${arrow}`,
      callback() {
        globals.dispatch(Actions.setPivotTableSortColumn({column, order}));
        globals.dispatch(
            Actions.setPivotTableQueryRequested({queryRequested: true}));
      },
    };
  }

  readableAggregationName(aggregation: Aggregation) {
    if (aggregation.aggregationFunction === 'COUNT') {
      return 'Count';
    }
    return `${aggregation.aggregationFunction}(${
        readableColumnName(aggregation.column)})`;
  }

  renderAggregationHeaderCell(aggregation: Aggregation): m.Child {
    const column = aggregation.column;
    const popupItems: PopupMenuItem[] = [];
    const state = globals.state.nonSerializableState.pivotTableRedux;
    if (state.sortCriteria === undefined ||
        !tableColumnEquals(column, state.sortCriteria.column)) {
      popupItems.push(
          this.sortingItem(column, 'DESC'), this.sortingItem(column, 'ASC'));
    } else {
      // Table is already sorted by the same column, return one item with
      // opposite direction.
      popupItems.push(this.sortingItem(
          column, state.sortCriteria.order === 'DESC' ? 'ASC' : 'DESC'));
    }
    const otherAggs: AggregationFunction[] = ['SUM', 'MAX', 'MIN'];
    if (aggregation.aggregationFunction !== 'COUNT') {
      for (const otherAgg of otherAggs) {
        if (aggregation.aggregationFunction === otherAgg) {
          continue;
        }

        popupItems.push({
          text: otherAgg,
          callback() {
            globals.dispatch(Actions.setPivotTableAggregationSelected(
                {column: aggregation, selected: false}));
            globals.dispatch(Actions.setPivotTableAggregationSelected({
              column:
                  {aggregationFunction: otherAgg, column: aggregation.column},
              selected: true,
            }));
            globals.dispatch(
                Actions.setPivotTableQueryRequested({queryRequested: true}));
          },
        });
      }
    }

    return m(
        'td', this.readableAggregationName(aggregation), m(PopupMenuButton, {
          icon: 'arrow_drop_down',
          items: popupItems,
        }));
  }

  showModal = false;
  typedArgument = '';

  renderModal(): ModalDefinition {
    return {
      title: 'Enter argument name',
      content: m(ArgumentPopup, {
                 knownArguments: globals.state.nonSerializableState
                                     .pivotTableRedux.argumentNames,
                 onArgumentChange: (arg) => {
                   this.typedArgument = arg;
                 },
               }) as AnyAttrsVnode,
      buttons: [
        {
          text: 'Add',
          action: () => {
            globals.dispatch(Actions.setPivotTablePivotSelected({
              column: {kind: 'argument', argument: this.typedArgument},
              selected: true,
            }));
            globals.dispatch(
                Actions.setPivotTableQueryRequested({queryRequested: true}));
          },
        },
      ],
    };
  }

  renderResultsTable(attrs: PivotTableReduxAttrs) {
    const state = globals.state.nonSerializableState.pivotTableRedux;
    if (state.queryResult === null) {
      return m('div', 'Loading...');
    }

    const renderedRows: m.Vnode[] = [];
    const tree = state.queryResult.tree;

    if (tree.children.size === 0 && tree.rows.length === 0) {
      // Empty result, render a special message
      return m('.empty-result', 'No slices in the current selection.');
    }

    this.renderTree(
        globals.state.areas[attrs.selectionArea.areaId],
        [],
        tree,
        state.queryResult,
        renderedRows);

    const pivotTableHeaders = [];
    for (const pivot of state.queryResult.metadata.pivotColumns) {
      const items = [{
        text: 'Add argument pivot',
        callback: () => {
          this.showModal = true;
          this.typedArgument = '';
          fullscreenModalContainer.createNew(this.renderModal());
        },
      }];
      if (state.queryResult.metadata.pivotColumns.length > 1) {
        items.push({
          text: 'Remove',
          callback() {
            globals.dispatch(Actions.setPivotTablePivotSelected(
                {column: pivot, selected: false}));
            globals.dispatch(
                Actions.setPivotTableQueryRequested({queryRequested: true}));
          },
        });
      }
      pivotTableHeaders.push(
          m('td',
            readableColumnName(pivot),
            m(PopupMenuButton, {icon: 'arrow_drop_down', items})));
    }

    const aggregationTableHeaders =
        state.queryResult.metadata.aggregationColumns.map(
            (aggregation) => this.renderAggregationHeaderCell(aggregation));

    return m(
        'table.query-table.pivot-table',
        m('thead',
          // First row of the table, containing names of pivot and aggregation
          // columns, as well as popup menus to modify the columns. Last cell
          // is empty because of an extra column with "drill down" button for
          // each pivot table row.
          m('tr', pivotTableHeaders, aggregationTableHeaders, m('td'))),
        m('tbody', this.renderTotalsRow(state.queryResult), renderedRows));
  }


  renderQuery(attrs: PivotTableReduxAttrs): m.Vnode {
    // Prepare a button to switch to results mode.
    let innerElement = m(
        'button.mode-button',
        {
          onclick: () => {
            globals.dispatch(Actions.setPivotTableEditMode({editMode: false}));
            globals.rafScheduler.scheduleFullRedraw();
          },
        },
        'Execute');
    try {
      this.generateQuery(attrs);
    } catch (e) {
      if (e instanceof QueryGeneratorError) {
        // If query generation fails, show an error message instead of a button.
        innerElement = m('div.query-error', e.message);
      } else {
        throw e;
      }
    }

    return m(
        'div',
        m('div',
          m('input', {
            type: 'checkbox',
            id: 'constrain-to-selection',
            checked: this.constrainToArea,
            onclick: (e: InputEvent) => {
              const checkbox = e.target as HTMLInputElement;
              globals.dispatch(Actions.setPivotTableReduxConstrainToArea(
                  {constrain: checkbox.checked}));
            },
          }),
          m('label',
            {
              'for': 'constrain-to-selection',
            },
            'Constrain to current time range')),
        innerElement);
  }

  view({attrs}: m.Vnode<PivotTableReduxAttrs>): m.Children {
    if (this.showModal) {
      fullscreenModalContainer.updateVdom(this.renderModal());
    }

    return globals.state.nonSerializableState.pivotTableRedux.editMode ?
        this.renderEditView(attrs) :
        this.renderResultsView(attrs);
  }

  renderEditView(attrs: PivotTableReduxAttrs) {
    return m(
        '.pivot-table-redux.edit',
        m('div',
          m('h2', 'Pivots'),
          m('ul',
            tables.map(
                (t) => this.renderTablePivotColumns(t),
                ))),
        m('div',
          m('h2', 'Aggregations'),
          m('ul',
            m('li',
              m(AggregationCheckbox, {
                get: this.selectedAggregations,
                set: Actions.setPivotTableAggregationSelected,
                setKey: COUNT_AGGREGATION,
              }),
              'count'),
            ...sliceAggregationColumns.map(
                (t) =>
                    m('li',
                      m(AggregationCheckbox, {
                        get: this.selectedAggregations,
                        set: Actions.setPivotTableAggregationSelected,
                        setKey: {
                          aggregationFunction: 'SUM',
                          column: {kind: 'regular', table: 'slice', column: t},
                        },
                      }),
                      t)),
            ...threadSliceAggregationColumns.map(
                (t) => m(
                    'li',
                    m(AggregationCheckbox, {
                      get: this.selectedAggregations,
                      set: Actions.setPivotTableAggregationSelected,
                      setKey: {
                        aggregationFunction: 'SUM',
                        column:
                            {kind: 'regular', table: 'thread_slice', column: t},
                      },
                    }),
                    `thread_slice.${t}`)))),
        this.renderQuery(attrs));
  }
}
