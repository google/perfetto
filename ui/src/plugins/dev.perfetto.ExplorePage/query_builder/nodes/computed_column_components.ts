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
import {ColumnInfo} from '../column_info';
import {Switch} from '../../../../widgets/switch';
import {OutlinedField, FormListItem, AddItemPlaceholder} from '../widgets';
import {NewColumn} from './add_columns_types';

/**
 * Attrs for the SwitchComponent.
 */
export interface SwitchComponentAttrs {
  column: NewColumn;
  columns: ColumnInfo[];
  onchange: () => void;
}

/**
 * Component for configuring a SWITCH/CASE computed column.
 * Allows selecting a column to switch on, defining cases with when/then pairs,
 * and an optional default value.
 */
export class SwitchComponent implements m.ClassComponent<SwitchComponentAttrs> {
  view({attrs}: m.Vnode<SwitchComponentAttrs>) {
    const {column, columns, onchange} = attrs;

    if (column.type !== 'switch') {
      return m('');
    }

    const setSwitchOn = (newSwitchOn: string) => {
      column.switchOn = newSwitchOn;
      this.updateExpression(column);
      onchange();
    };

    const setDefaultValue = (newDefaultValue: string) => {
      column.defaultValue = newDefaultValue;
      this.updateExpression(column);
      onchange();
    };

    const setCaseWhen = (index: number, newWhen: string) => {
      if (!column.cases) return;
      column.cases[index].when = newWhen;
      this.updateExpression(column);
      onchange();
    };

    const setCaseThen = (index: number, newThen: string) => {
      if (!column.cases) return;
      column.cases[index].then = newThen;
      this.updateExpression(column);
      onchange();
    };

    const addCase = () => {
      if (!column.cases) {
        column.cases = [];
      }
      column.cases.push({when: '', then: ''});
      this.updateExpression(column);
      onchange();
    };

    const removeCase = (index: number) => {
      if (!column.cases) return;
      column.cases.splice(index, 1);
      this.updateExpression(column);
      onchange();
    };

    if (column.switchOn === undefined || column.switchOn === '') {
      const columnNames = columns.map((c) => c.column.name);
      return m(
        OutlinedField,
        {
          label: 'Switch on column',
          value: '',
          onchange: (e: Event) => {
            setSwitchOn((e.target as HTMLSelectElement).value);
          },
        },
        [
          m('option', {value: ''}, 'Select column'),
          ...columnNames.map((name) => m('option', {value: name}, name)),
        ],
      );
    }

    const columnNames = columns.map((c) => c.column.name);

    const selectedColumn = columns.find(
      (c) => c.column.name === column.switchOn,
    );
    const isStringColumn = selectedColumn?.type === 'STRING';

    return m('.pf-inline-edit-list', [
      m(
        OutlinedField,
        {
          label: 'Switch on column',
          value: column.switchOn,
          onchange: (e: Event) => {
            setSwitchOn((e.target as HTMLSelectElement).value);
          },
        },
        columnNames.map((name) => m('option', {value: name}, name)),
      ),
      isStringColumn &&
        m(Switch, {
          label: 'Use glob matching',
          checked: column.useGlob ?? false,
          onchange: (e: Event) => {
            column.useGlob = (e.target as HTMLInputElement).checked;
            this.updateExpression(column);
            onchange();
          },
        }),
      m(OutlinedField, {
        label: 'Default value',
        placeholder: 'default value',
        value: column.defaultValue || '',
        oninput: (e: Event) => {
          setDefaultValue((e.target as HTMLInputElement).value);
        },
      }),
      ...(column.cases || []).map((c, i) =>
        m(FormListItem, {
          item: c,
          isValid: c.when.trim() !== '' && c.then.trim() !== '',
          onUpdate: () => {},
          onRemove: () => removeCase(i),
          children: [
            m(OutlinedField, {
              label: 'When',
              placeholder: 'is equal to',
              value: c.when,
              oninput: (e: Event) => {
                setCaseWhen(i, (e.target as HTMLInputElement).value);
              },
            }),
            m(OutlinedField, {
              label: 'Then',
              placeholder: 'then value',
              value: c.then,
              oninput: (e: Event) => {
                setCaseThen(i, (e.target as HTMLInputElement).value);
              },
            }),
          ],
        }),
      ),
      m(AddItemPlaceholder, {
        label: 'Add case',
        icon: 'add',
        onclick: addCase,
      }),
    ]);
  }

  private updateExpression(col: NewColumn) {
    if (col.type !== 'switch' || !col.switchOn) {
      col.expression = '';
      return;
    }

    const operator = col.useGlob ? 'GLOB' : '=';
    const casesStr = (col.cases || [])
      .filter((c) => c.when.trim() !== '' && c.then.trim() !== '')
      .map((c) => `WHEN ${col.switchOn} ${operator} ${c.when} THEN ${c.then}`)
      .join(' ');

    const defaultStr = col.defaultValue ? `ELSE ${col.defaultValue}` : '';

    if (casesStr === '' && defaultStr === '') {
      col.expression = '';
      return;
    }

    col.expression = `CASE ${casesStr} ${defaultStr} END`;
  }
}

/**
 * Attrs for the IfComponent.
 */
export interface IfComponentAttrs {
  column: NewColumn;
  onchange: () => void;
}

/**
 * Component for configuring an IF/ELSE computed column.
 * Allows defining multiple IF/ELSE IF conditions with then values,
 * and an optional ELSE clause.
 */
export class IfComponent implements m.ClassComponent<IfComponentAttrs> {
  view({attrs}: m.Vnode<IfComponentAttrs>) {
    const {column, onchange} = attrs;

    if (column.type !== 'if') {
      return m('');
    }

    const setIfCondition = (index: number, newIf: string) => {
      if (!column.clauses) return;
      column.clauses[index].if = newIf;
      this.updateExpression(column);
      onchange();
    };

    const setThenValue = (index: number, newThen: string) => {
      if (!column.clauses) return;
      column.clauses[index].then = newThen;
      this.updateExpression(column);
      onchange();
    };

    const setElseValue = (newElse: string) => {
      column.elseValue = newElse;
      this.updateExpression(column);
      onchange();
    };

    const addElseIf = () => {
      if (!column.clauses) {
        column.clauses = [];
      }
      column.clauses.push({if: '', then: ''});
      this.updateExpression(column);
      onchange();
    };

    const removeClause = (index: number) => {
      if (!column.clauses) return;
      column.clauses.splice(index, 1);
      this.updateExpression(column);
      onchange();
    };

    const hasElse = column.elseValue !== undefined;

    return m('.pf-inline-edit-list', [
      ...(column.clauses || []).map((c, i) =>
        m(FormListItem, {
          item: c,
          isValid: c.if.trim() !== '' && c.then.trim() !== '',
          onUpdate: () => {},
          onRemove: () => removeClause(i),
          children: [
            m(OutlinedField, {
              label: i === 0 ? 'If' : 'Else If',
              placeholder: 'condition',
              value: c.if,
              oninput: (e: Event) => {
                setIfCondition(i, (e.target as HTMLInputElement).value);
              },
            }),
            m(OutlinedField, {
              label: 'Then',
              placeholder: 'value',
              value: c.then,
              oninput: (e: Event) => {
                setThenValue(i, (e.target as HTMLInputElement).value);
              },
            }),
          ],
        }),
      ),
      hasElse &&
        m(OutlinedField, {
          label: 'Else',
          placeholder: 'value',
          value: column.elseValue || '',
          oninput: (e: Event) => {
            setElseValue((e.target as HTMLInputElement).value);
          },
        }),
      !hasElse &&
        m(AddItemPlaceholder, {
          label: 'Add ELSE IF',
          icon: 'add',
          onclick: addElseIf,
        }),
      !hasElse &&
        m(AddItemPlaceholder, {
          label: 'Add ELSE',
          icon: 'add',
          onclick: () => {
            column.elseValue = '';
            this.updateExpression(column);
            onchange();
          },
        }),
    ]);
  }

  private updateExpression(col: NewColumn) {
    if (col.type !== 'if') {
      col.expression = '';
      return;
    }

    const clausesStr = (col.clauses || [])
      .filter((c) => c.if.trim() !== '' && c.then.trim() !== '')
      .map((c) => `WHEN ${c.if} THEN ${c.then}`)
      .join(' ');

    const elseStr =
      col.elseValue !== undefined ? `ELSE ${col.elseValue.trim()}` : '';

    if (clausesStr === '' && elseStr === '') {
      col.expression = '';
      return;
    }

    col.expression = `CASE ${clausesStr} ${elseStr} END`;
  }
}
