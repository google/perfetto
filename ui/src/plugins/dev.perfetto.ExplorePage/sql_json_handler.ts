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

import {NodeType} from './query_node';
import {Trace} from '../../public/trace';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import {ExplorePageState} from './explore_page';
import {showModal, closeModal} from '../../widgets/modal';
import {Editor} from '../../widgets/editor';
import {
  SerializedGraph,
  SerializedNode,
  deserializeState,
} from './json_handler';
import {SqlSourceSerializedState} from './query_builder/nodes/sources/sql_source';

export function showImportWithStatementModal(
  trace: Trace,
  sqlModules: SqlModules,
  onStateUpdate: (state: ExplorePageState) => void,
) {
  let sqlText = '';
  showModal({
    title: 'Import from WITH statement',
    content: m(
      'div',
      {
        style: {
          'border-top': '1px solid var(--pf-color-border)',
          'min-height': '10rem',
          'overflow': 'hidden auto',
        },
      },
      m(Editor, {
        text: sqlText,
        onUpdate: (text: string) => {
          sqlText = text;
        },
      }),
    ),
    buttons: [
      {
        text: 'Import',
        action: () => {
          const json = createGraphFromSql(sqlText);
          const newState = deserializeState(json, trace, sqlModules);
          onStateUpdate(newState);
          closeModal();
        },
      },
      {
        text: 'Cancel',
        action: () => {
          closeModal();
        },
      },
    ],
  });
}

// Simplified representation of a query node for parsing
interface ParsedNode {
  name: string;
  query: string;
  dependencies: string[];
}

interface ParsedSql {
  nodes: ParsedNode[];
  modules: string;
}

function parseSqlWithModules(sql: string): ParsedSql {
  const modules: string[] = [];
  const lines = sql.split('\n');
  let lastModuleLine = -1;

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (line.toUpperCase().startsWith('PERFETTO INCLUDE MODULE')) {
      modules.push(line);
      lastModuleLine = i;
    } else if (line !== '' && !line.startsWith('--')) {
      break;
    }
  }

  const sqlWithoutModules = lines.slice(lastModuleLine + 1).join('\n');
  const nodes = parseSql(sqlWithoutModules);

  return {
    nodes,
    modules: modules.join('\n'),
  };
}

function parseSql(sql: string): ParsedNode[] {
  // TODO(mayzner): This whole logic is very fragile and should be replaced
  // with Trace Processor SQL parser, when it becomes available.
  const nodes: ParsedNode[] = [];
  const sqlUpperCase = sql.toUpperCase();
  const withIndex = sqlUpperCase.indexOf('WITH');
  if (withIndex === -1) {
    return nodes;
  }

  let openParens = 0;
  let selectIndex = -1;

  for (let i = withIndex + 4; i < sql.length; i++) {
    if (sql[i] === '(') {
      openParens++;
    } else if (sql[i] === ')') {
      openParens--;
    } else if (
      openParens === 0 &&
      sqlUpperCase.substring(i).startsWith('SELECT')
    ) {
      selectIndex = i;
      break;
    }
  }

  if (selectIndex === -1) {
    throw new Error(
      'Malformed SQL: No SELECT statement found after WITH clause.',
    );
  }

  const withClause = sql.substring(withIndex + 4, selectIndex).trim();

  openParens = 0;
  let lastSplit = 0;
  const parts: string[] = [];

  for (let i = 0; i < withClause.length; i++) {
    if (withClause[i] === '(') {
      openParens++;
    } else if (withClause[i] === ')') {
      openParens--;
    } else if (withClause[i] === ',' && openParens === 0) {
      parts.push(withClause.substring(lastSplit, i));
      lastSplit = i + 1;
    }
  }
  parts.push(withClause.substring(lastSplit));

  for (const part of parts) {
    const asMatch = part.trim().match(/([a-zA-Z0-9_]+)\s+AS\s+\((.*)\)/is);
    if (!asMatch) {
      throw new Error(`Malformed CTE clause: ${part}`);
    }

    const name = asMatch[1].trim();
    let query = asMatch[2].trim();
    const dependencies = [];

    for (const node of nodes) {
      const regex = new RegExp(`\\b${node.name}\\b`, 'g');
      if (query.match(regex)) {
        dependencies.push(node.name);
        query = query.replace(regex, `$${node.name}`);
      }
    }

    nodes.push({
      name,
      query,
      dependencies,
    });
  }

  // Handle the final SELECT statement
  const finalSelectQuery = sql.substring(selectIndex).trim();
  let finalQueryWithVars = finalSelectQuery;
  const finalDependencies = [];

  for (const node of nodes) {
    const regex = new RegExp(`\\b${node.name}\\b`, 'g');
    if (finalQueryWithVars.match(regex)) {
      finalDependencies.push(node.name);
      finalQueryWithVars = finalQueryWithVars.replace(regex, `$${node.name}`);
    }
  }

  let finalNodeName = 'output';
  let counter = 1;
  const existingNames = new Set(nodes.map((n) => n.name));
  while (existingNames.has(finalNodeName)) {
    finalNodeName = `output_${counter}`;
    counter++;
  }

  nodes.push({
    name: finalNodeName,
    query: finalQueryWithVars,
    dependencies: finalDependencies,
  });

  return nodes;
}

export function createGraphFromSql(sql: string): string {
  const {nodes: parsedNodes, modules} = parseSqlWithModules(sql);
  const serializedNodes: SerializedNode[] = [];
  const nodeLayouts: {[key: string]: {x: number; y: number}} = {};
  const rootNodeIds: string[] = [];

  const nodeMap = new Map<string, SerializedNode>();

  for (const parsedNode of parsedNodes) {
    const nodeId = parsedNode.name;
    const node: SerializedNode = {
      nodeId,
      type: NodeType.kSqlSource,
      state: {
        sql: parsedNode.query,
        filters: [],
      },
      nextNodes: [],
      prevNodes: [],
    };
    serializedNodes.push(node);
    nodeMap.set(nodeId, node);
  }

  for (const parsedNode of parsedNodes) {
    const node = nodeMap.get(parsedNode.name)!;
    for (const dep of parsedNode.dependencies) {
      const depNode = nodeMap.get(dep)!;
      depNode.nextNodes.push(node.nodeId);
      if (!node.prevNodes) {
        node.prevNodes = [];
      }
      node.prevNodes.push(depNode.nodeId);
    }
    if (parsedNode.dependencies.length === 0) {
      rootNodeIds.push(node.nodeId);
    }
  }

  if (modules && rootNodeIds.length > 0) {
    const firstRootNode = nodeMap.get(rootNodeIds[0])!;
    if (firstRootNode.type === NodeType.kSqlSource) {
      const state = firstRootNode.state as SqlSourceSerializedState;
      state.sql = `${modules}\n${state.sql}`;
    }
  }

  const serializedGraph: SerializedGraph = {
    nodes: serializedNodes,
    rootNodeIds,
    nodeLayouts,
  };

  return JSON.stringify(serializedGraph, null, 2);
}
