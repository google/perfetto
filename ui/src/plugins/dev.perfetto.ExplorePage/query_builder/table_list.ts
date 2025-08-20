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
import {FuzzyFinder} from '../../../base/fuzzy';
import {SqlModules, SqlTable} from '../../dev.perfetto.SqlModules/sql_modules';
import {Card, CardStack} from '../../../widgets/card';
import {EmptyState} from '../../../widgets/empty_state';

export interface TableListAttrs {
  sqlModules: SqlModules;
  onTableClick: (tableName: string) => void;
  searchQuery: string;
  onSearchQueryChange: (query: string) => void;
  autofocus?: boolean;
}

interface TableWithModule {
  table: SqlTable;
  moduleName: string;
}

export class TableList implements m.ClassComponent<TableListAttrs> {
  view({attrs}: m.CVnode<TableListAttrs>) {
    const modules = attrs.sqlModules.listModules();

    const allTables: TableWithModule[] = modules.flatMap((module) =>
      module.tables.map((table) => ({
        table,
        moduleName: module.includeKey,
      })),
    );

    const finder = new FuzzyFinder(allTables, (item) => item.table.name);
    const fuzzyResults = finder.find(attrs.searchQuery);

    const tableList = fuzzyResults.map(({item, segments}) => {
      const renderedName = segments.map((segment) => {
        return m(segment.matching ? 'strong' : 'span', segment.value);
      });
      const packageName = item.moduleName.split('.')[0];
      return m(
        Card,
        {
          onclick: () => attrs.onTableClick(item.table.name),
          interactive: true,
        },
        m(
          '.pf-table-card',
          m('.table-name', renderedName),
          packageName === 'prelude'
            ? null
            : m('.table-module', item.moduleName),
          item.table.description &&
            m('.table-description', item.table.description),
        ),
      );
    });

    const content =
      tableList.length > 0
        ? m(CardStack, tableList)
        : m(EmptyState, {title: 'No tables found'});

    return m(
      '.pf-table-list',
      m('input[type=text].pf-search', {
        placeholder: 'Search Perfetto SQL tables...',
        oninput: (e: Event) => {
          attrs.onSearchQueryChange((e.target as HTMLInputElement).value);
        },
        value: attrs.searchQuery,
        oncreate: (vnode) => {
          if (attrs.autofocus) {
            (vnode.dom as HTMLInputElement).focus();
          }
        },
      }),
      m(CardStack, content),
    );
  }
}
