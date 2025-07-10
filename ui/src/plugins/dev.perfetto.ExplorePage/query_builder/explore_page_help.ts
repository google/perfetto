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
import {SqlModules, SqlTable} from '../../dev.perfetto.SqlModules/sql_modules';

export interface ExplorePageHelpAttrs {
  sqlModules: SqlModules;
  onTableClick: (tableName: string) => void;
}

interface ModuleTreeNode {
  name: string;
  children: Map<string, ModuleTreeNode>;
  tables: SqlTable[];
}

export class ExplorePageHelp implements m.ClassComponent<ExplorePageHelpAttrs> {
  private searchQuery = '';

  view({attrs}: m.CVnode<ExplorePageHelpAttrs>) {
    return m(
      '.pf-node-explorer-help',
      this.renderGettingStarted(),
      this.renderSqlModules(attrs),
    );
  }

  private renderGettingStarted() {
    return m(
      '.getting-started',
      m('h3', 'How to get started:'),
      m(
        '.step-cards',
        m(
          '.card',
          m('h4', '1. Add a source node'),
          m(
            'p',
            'Begin by adding a new source node from the panel on the left.',
          ),
        ),
        m(
          '.card',
          m('h4', '2. Configure the node'),
          m(
            'p',
            'Click on a node to open its configuration options in this panel.',
          ),
        ),
        m(
          '.card',
          m('h4', '3. View the results'),
          m(
            'p',
            'The output of the selected node will be shown in the table below.',
          ),
        ),
      ),
    );
  }

  private renderSqlModules({sqlModules, onTableClick}: ExplorePageHelpAttrs) {
    const modules = sqlModules.listModules();

    const root: ModuleTreeNode = {
      name: 'root',
      children: new Map(),
      tables: [],
    };

    for (const module of modules) {
      const parts = module.includeKey.split('.');
      let currentNode = root;
      for (const part of parts) {
        if (!currentNode.children.has(part)) {
          currentNode.children.set(part, {
            name: part,
            children: new Map(),
            tables: [],
          });
        }
        currentNode = currentNode.children.get(part)!;
      }
      currentNode.tables.push(...module.tables);
    }

    const renderNode = (node: ModuleTreeNode): m.Children => {
      const children = Array.from(node.children.values())
        .map((child) => {
          const renderedChild = renderNode(child);
          if (renderedChild === null) {
            return null;
          }
          return m('li', m('strong', child.name), renderedChild);
        })
        .filter((c) => c !== null);

      const lowerCaseQuery = this.searchQuery.toLowerCase();
      const tables = node.tables
        .filter((table) => table.name.toLowerCase().includes(lowerCaseQuery))
        .map((table) => {
          return m(
            'li.clickable',
            {onclick: () => onTableClick(table.name)},
            m('div.table-name', table.name),
            table.description && m('div.table-description', table.description),
          );
        });

      if (children.length === 0 && tables.length === 0) {
        return null;
      }

      return m('ul', ...children, ...tables);
    };

    return m(
      '.sql-modules',
      m('h3', 'Perfetto SQL Tables'),
      m('input[type=text].pf-search', {
        placeholder: 'Search tables...',
        oninput: (e: Event) => {
          this.searchQuery = (e.target as HTMLInputElement).value;
        },
        value: this.searchQuery,
      }),
      m('.module-tree', renderNode(root)),
    );
  }
}
