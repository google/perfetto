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
import {
  SqlColumn,
  SqlModules,
  SqlTable,
  TableAndColumn,
} from '../../../dev.perfetto.SqlModules/sql_modules';
import {NodeType, QueryNode} from '../../query_state';
import {
  ColumnController,
  ColumnControllerDiff,
  ColumnControllerRows,
} from '../column_controller';
import {CheckboxAttrs, Checkbox} from '../../../../widgets/checkbox';
import {Section} from '../../../../widgets/section';
import {Select} from '../../../../widgets/select';
import {TextParagraph} from '../../../../widgets/text_paragraph';
import protos from '../../../../protos';

export interface JoinOperationAttrs {
  readonly sqlModules: SqlModules;
  joinState: JoinState;
}

export class JoinState implements QueryNode {
  type: NodeType = NodeType.kJoinOperator;
  prevNode: QueryNode;
  nextNode?: QueryNode;
  finished = false;

  dataName = undefined;
  cte: boolean;
  imports?: string[];
  columns?: ColumnControllerRows[];

  joinColumn?: SqlColumn;

  secondaryTable?: SqlTable;
  secondaryJoinColumn?: SqlColumn;

  primaryColumnsPicked?: ColumnControllerRows[];
  secondaryColumnsPicked?: ColumnControllerRows[];

  constructor(prevNode: QueryNode) {
    this.prevNode = prevNode;
    this.cte = false;
  }
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    throw new Error('Method not implemented.');
  }

  getTitle(): string {
    if (this.finished === false || this.secondaryTable === undefined) {
      return '';
    }
    return `JOIN with ${this.secondaryTable.name}`;
  }

  getSourceSql(): string | undefined {
    if (
      this.primaryColumnsPicked === undefined ||
      this.secondaryColumnsPicked === undefined
    ) {
      return;
    }

    return `JOIN ${this.secondaryTable?.name} ON ${this.prevNode.dataName}.${this.joinColumn?.name}=${this.secondaryTable?.name}.${this.secondaryJoinColumn?.name}`;
  }

  validate(): boolean {
    if (
      findCollisions(this) === undefined ||
      findCollisions(this)?.length !== 0
    ) {
      return false;
    }
    const primaryPicked = this.primaryColumnsPicked?.filter((c) => c.checked);
    const secondaryPicked = this.secondaryColumnsPicked?.filter(
      (c) => c.checked,
    );
    this.columns = primaryPicked?.concat(secondaryPicked || []);
    this.finished = true;
    return true;
  }
}

export class QueryBuilderJoin implements m.ClassComponent<JoinOperationAttrs> {
  view({attrs}: m.CVnode<JoinOperationAttrs>) {
    if (attrs.joinState.prevNode.columns === undefined) {
      return;
    }

    const selectJoinColumn = (): m.Child => {
      return m(
        Select,
        {
          id: 'choose_join_id_col',
          onchange: (e: Event) => {
            if (attrs.joinState.prevNode.columns === undefined) {
              return;
            }
            const colName = (e.target as HTMLSelectElement).value;
            for (const c of attrs.joinState.prevNode.columns) {
              if (c.column.name === colName) {
                Object.assign(
                  attrs.joinState,
                  new JoinState(attrs.joinState.prevNode),
                );
                attrs.joinState.joinColumn = c.column;
                return;
              }
            }
          },
          value: attrs.joinState.joinColumn?.name,
        },
        attrs.joinState.prevNode.columns &&
          getJoinIdColumns(attrs.joinState.prevNode.columns).map((c) =>
            m('option', c.name),
          ),
      );
    };

    const primaryTableJoinIdCols = getJoinIdColumns(
      attrs.joinState.prevNode.columns,
    );
    const joinIdColsStr = primaryTableJoinIdCols
      .map(
        (c) =>
          `${c.name} (with '${c.type.tableAndColumn?.table}' on '${c.type.tableAndColumn?.column}')`,
      )
      .join(', ');

    const pickColumnsForJoin = (): m.Child => {
      if (
        attrs.joinState.secondaryTable === undefined ||
        attrs.joinState.prevNode.columns === undefined
      ) {
        return;
      }

      if (
        attrs.joinState.primaryColumnsPicked === undefined ||
        attrs.joinState.primaryColumnsPicked.length !==
          attrs.joinState.prevNode.columns.filter((c) => c.checked).length
      ) {
        attrs.joinState.primaryColumnsPicked = [];
        attrs.joinState.prevNode.columns
          .filter((c) => c.checked)
          .forEach((val) =>
            attrs.joinState.primaryColumnsPicked?.push(Object.assign({}, val)),
          );
        attrs.joinState.primaryColumnsPicked.map(
          (c) => (c.source = attrs.joinState.prevNode.dataName),
        );
      }

      if (attrs.joinState.secondaryColumnsPicked === undefined) {
        attrs.joinState.secondaryColumnsPicked =
          attrs.joinState.secondaryTable.columns.map(
            (c) => new ColumnControllerRows(c),
          );
        attrs.joinState.secondaryColumnsPicked.map(
          (c) => (c.source = attrs.joinState.secondaryTable?.name),
        );
      }

      return m(
        '.explore-page__rowish',
        // Primary table columns
        m(
          Section,
          {title: `From ${attrs.joinState.prevNode.getTitle()}`},
          m(ColumnController, {
            options: attrs.joinState.primaryColumnsPicked,
            hasValidColumns: true,
            onChange: (diffs: ColumnControllerDiff[]) => {
              diffs.forEach(({id, checked, alias}) => {
                if (attrs.joinState.primaryColumnsPicked === undefined) {
                  return;
                }
                for (const option of attrs.joinState.primaryColumnsPicked) {
                  if (option.id == id) {
                    option.checked = checked;
                    option.alias = alias;
                  }
                }
              });
            },
          }),
        ),
        // Secondary table columns
        m(
          Section,
          {
            title: `From table: ${attrs.joinState.secondaryTable.name}`,
          },
          m(ColumnController, {
            options: attrs.joinState.secondaryColumnsPicked,
            hasValidColumns: true,
            onChange: (diffs: ColumnControllerDiff[]) => {
              diffs.forEach(({id, checked, alias}) => {
                if (attrs.joinState.secondaryColumnsPicked === undefined) {
                  return;
                }
                for (const option of attrs.joinState.secondaryColumnsPicked) {
                  if (option.id == id) {
                    option.checked = checked;
                    option.alias = alias;
                  }
                }
              });
            },
          }),
        ),
      );
    };

    function renderCollisions(): m.Child {
      const collisions = findCollisions(attrs.joinState);

      if (collisions === undefined || collisions.length === 0) {
        return;
      }

      return m(TextParagraph, {
        text: `Column names present in both tables: ${collisions.join(', ')}.\
         All names have to be unique.`,
      });
    }

    return m(
      '.explore-page__columnar',
      m(TextParagraph, {text: `JoinId columns: ${joinIdColsStr}`}),
      selectJoinColumn(),
      attrs.joinState.joinColumn &&
        m(SecondaryTableAndColumnSelector, {
          primaryJoinCol: attrs.joinState.joinColumn,
          sqlModules: attrs.sqlModules,
          onTablePicked: (arg) => {
            const newJoinState = new JoinState(attrs.joinState.prevNode);
            newJoinState.joinColumn = attrs.joinState.joinColumn;
            newJoinState.secondaryTable = arg;
            newJoinState.imports = arg.includeKey ? [arg.includeKey] : [];
            Object.assign(attrs.joinState, newJoinState);
          },
          onColumnPicked: (arg) => {
            const newJoinState = new JoinState(attrs.joinState.prevNode);
            newJoinState.joinColumn = attrs.joinState.joinColumn;
            newJoinState.secondaryTable = attrs.joinState.secondaryTable;
            newJoinState.imports = attrs.joinState.imports;
            newJoinState.secondaryJoinColumn = arg;
            Object.assign(attrs.joinState, newJoinState);
          },
        }),
      pickColumnsForJoin(),
      renderCollisions(),
    );
  }
}

interface SecondaryTableAndColumnPickerAttrs {
  readonly primaryJoinCol: SqlColumn;
  readonly sqlModules: SqlModules;

  readonly onTablePicked: (table: SqlTable) => void;
  readonly onColumnPicked: (column: SqlColumn) => void;
}

class SecondaryTableAndColumnSelector
  implements m.ClassComponent<SecondaryTableAndColumnPickerAttrs>
{
  private trivialOrLinkedTable?: 'trivial' | 'linked';

  private selectedTable?: SqlTable;

  view({attrs}: m.CVnode<SecondaryTableAndColumnPickerAttrs>) {
    function getLinkedIdTables(): SqlTable[] {
      return attrs.primaryJoinCol.type.tableAndColumn
        ? attrs.sqlModules.findAllTablesWithLinkedId(
            attrs.primaryJoinCol.type.tableAndColumn,
          )
        : [];
    }

    if (attrs.primaryJoinCol.type.tableAndColumn === undefined) {
      return;
    }

    const selectIfLinkedId = (): m.Child => {
      const linkedIdTables = getLinkedIdTables();
      return m(
        Select,
        {
          id: 'join_id_col_chosen',
          onchange: (e: Event) => {
            const tableName = (e.target as HTMLSelectElement).value;
            for (const t of linkedIdTables) {
              if (t.name === tableName) {
                this.selectedTable = t;
                attrs.onTablePicked(t);
              }
            }
            if (this.selectedTable !== undefined) {
              const bla = getLinkedIdColumn(
                this.selectedTable,
                attrs.primaryJoinCol,
              );
              if (bla) {
                attrs.onColumnPicked(bla);
              }
            }
          },
          value: this.selectedTable?.name,
        },
        linkedIdTables.map((t) => m('option', t.name)),
      );
    };

    const trivialJoinCheckbox = (
      primaryTableAndColumn: TableAndColumn,
    ): CheckboxAttrs => {
      return {
        checked: this.trivialOrLinkedTable === 'trivial',
        label: `Trivial join on '${primaryTableAndColumn.table}'`,
        onchange: () => {
          if (primaryTableAndColumn === undefined) {
            return;
          }
          if (this.trivialOrLinkedTable === 'trivial') {
            this.trivialOrLinkedTable = undefined;
            this.selectedTable = undefined;
            return;
          }
          this.trivialOrLinkedTable = 'trivial';
          const t = attrs.sqlModules.getTable(primaryTableAndColumn.table);
          this.selectedTable = t;
          if (t === undefined) {
            return;
          }
          attrs.onTablePicked(t);
          const pickedCol = t.columns.find(
            (c) => c.name === primaryTableAndColumn.column,
          );
          pickedCol && attrs.onColumnPicked(pickedCol);
        },
      };
    };

    const joinCol = attrs.primaryJoinCol;
    return m(
      '',
      m(
        '',
        joinCol.type.tableAndColumn &&
          m(Checkbox, trivialJoinCheckbox(joinCol.type.tableAndColumn)),
      ),
      getLinkedIdTables().length !== 0 &&
        m(
          '',
          m(Checkbox, {
            checked: this.trivialOrLinkedTable === 'linked',
            label: `Linked join`,
            onchange: () => {
              if (this.trivialOrLinkedTable === 'linked') {
                this.trivialOrLinkedTable = undefined;
                this.selectedTable = undefined;
              } else {
                this.trivialOrLinkedTable = 'linked';
              }
            },
          }),
          this.trivialOrLinkedTable === 'linked' && selectIfLinkedId(),
        ),
    );
  }
}

function getJoinIdColumns(cols: ColumnControllerRows[]): SqlColumn[] {
  return cols
    .filter((c) => c.checked && c.column.type.shortName === 'joinid')
    .map((c) => c.column);
}

function getLinkedIdColumn(
  t: SqlTable,
  joinCol: SqlColumn,
): SqlColumn | undefined {
  if (joinCol.type.tableAndColumn === undefined) {
    return;
  }

  for (const c of t.columns) {
    if (
      c.type.shortName === 'id' &&
      c.type.tableAndColumn !== undefined &&
      c.type.tableAndColumn.isEqual(joinCol.type.tableAndColumn)
    ) {
      return c;
    }
  }
  return;
}

function getPickedColumnNames(rows: ColumnControllerRows[]): string[] {
  return rows.filter((col) => col.checked).map((col) => col.column.name);
}

function findNameCollisions(
  primaryCols: ColumnControllerRows[],
  secondaryCols: ColumnControllerRows[],
): string[] {
  const primaryNames = new Set(getPickedColumnNames(primaryCols));
  return secondaryCols
    .filter((col) => col.checked && primaryNames.has(col.column.name))
    .map((c) => c.column.name);
}

function findCollisions(joinState: JoinState): string[] | undefined {
  if (
    joinState.primaryColumnsPicked === undefined ||
    joinState.secondaryColumnsPicked === undefined
  ) {
    return;
  }
  return findNameCollisions(
    joinState.primaryColumnsPicked,
    joinState.secondaryColumnsPicked,
  );
}
