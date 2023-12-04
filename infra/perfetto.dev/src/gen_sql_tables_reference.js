// Copyright (C) 2020 The Android Open Source Project
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

// Generation of SQL table references from C++ headers.

'use strict';

const fs = require('fs');
const argv = require('yargs').argv

// Removes \n due to 80col wrapping and preserves only end-of-sentence line
// breaks.
// TODO dedupe, this is copied from the other gen_proto file.
function singleLineComment(comment) {
  comment = comment || '';
  comment = comment.trim();
  comment = comment.replaceAll('|', '\\|');
  comment = comment.replace(/\.\n/g, '<br>');
  comment = comment.replace(/\n/g, ' ');
  return comment;
}

function parseTablesInJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'UTF8'));
}

function genLink(table) {
  return `[${table.name}](#${table.name})`;
}

function tableToMarkdown(table) {
  let md = `### ${table.name}\n\n`;
  if (table.parent) {
    md += `_Extends ${genLink(table.parent)}_\n\n`;
  }
  md += table.comment + '\n\n';
  md += 'Column | Type | Description\n';
  md += '------ | ---- | -----------\n';

  let curTable = table;
  while (curTable) {
    if (curTable != table) {
      md += `||_Columns inherited from_ ${genLink(curTable)}\n`
    }
    for (const col of Object.values(curTable.cols)) {
      const type = col.type + (col.optional ? '<br>`optional`' : '');
      let description = col.comment;
      if (col.joinTable) {
        description += `\nJoinable with ` +
            `[${col.joinTable}.${col.joinCol}](#${col.joinTable})`;
      }
      md += `${col.name} | ${type} | ${singleLineComment(description)}\n`
    }
    curTable = curTable.parent;
  }
  md += '\n\n';
  return md;
}

function main() {
  const outFile = argv['o'];
  const jsonFile = argv['j'];
  if (!jsonFile) {
    console.error('Usage: -j tbls.json -[-o out.md]');
    process.exit(1);
  }

  // Can be either a string (-j single) or an array (-j one -j two).
  const jsonFiles = (jsonFile instanceof Array) ? jsonFile : [jsonFile];
  const jsonTables =
      Array.prototype.concat(...jsonFiles.map(parseTablesInJson));

  // Resolve parents.
  const tablesIndex = {};    // 'TP_SCHED_SLICE_TABLE_DEF' -> table
  const tablesByGroup = {};  // 'profilers' => [table1, table2]
  const tablesCppName = {};  // 'StackProfileMappingTable' => table
  const tablesByName = {};   // 'profile_mapping' => table
  for (const table of jsonTables) {
    tablesIndex[table.defMacro] = table;
    if (tablesByGroup[table.tablegroup] === undefined) {
      tablesByGroup[table.tablegroup] = [];
    }
    tablesCppName[table.cppClassName] = table;
    tablesByName[table.name] = table;
    tablesByGroup[table.tablegroup].push(table);
  }
  const tableGroups = Object.keys(tablesByGroup).sort((a, b) => {
    const keys = {'Tracks': '1', 'Events': '2', 'Misc': 'z'};
    a = `${keys[a]}_${a}`;
    b = `${keys[b]}_${b}`;
    return a.localeCompare(b);
  });

  for (const table of jsonTables) {
    if (table.parentDefName) {
      table.parent = tablesIndex[table.parentDefName];
    }
  }

  // Builds a graph of the tables' relationship that can be rendererd with
  // mermaid.js.
  let graph = '## Tables diagram\n';
  const mkLabel = (table) => `${table.defMacro}["${table.name}"]`;
  for (const tableGroup of tableGroups) {
    let graphEdges = '';
    let graphLinks = '';
    graph += `#### ${tableGroup} tables\n`;
    graph += '```mermaid\ngraph TD\n';
    graph += `  subgraph ${tableGroup}\n`;
    for (const table of tablesByGroup[tableGroup]) {
      graph += `  ${mkLabel(table)}\n`;
      graphLinks += `  click ${table.defMacro} "#${table.name}"\n`
      if (table.parent) {
        graphEdges += ` ${mkLabel(table)} --> ${mkLabel(table.parent)}\n`
      }

      for (const col of Object.values(table.cols)) {
        let refTable = undefined;
        if (col.refTableCppName) {
          refTable = tablesCppName[col.refTableCppName];
        } else if (col.joinTable) {
          refTable = tablesByName[col.joinTable];
          if (!refTable) {
            throw new Error(`Cannot find @joinable table ${col.joinTable}`);
          }
        }
        if (!refTable)
          continue;
        graphEdges +=
            `  ${mkLabel(table)} -. ${col.name} .-> ${mkLabel(refTable)}\n`
        graphLinks += `  click ${refTable.defMacro} "#${refTable.name}"\n`
      }
    }
    graph += `  end\n`;
    graph += graphEdges;
    graph += graphLinks;
    graph += '\n```\n';
  }

  let title = '# PerfettoSQL Prelude\n'
  let md = title + graph;
  for (const tableGroup of tableGroups) {
    md += `## ${tableGroup}\n`
    for (const table of tablesByGroup[tableGroup]) {
      md += tableToMarkdown(table);
    }
  }

  if (outFile) {
    fs.writeFileSync(outFile, md);
  } else {
    console.log(md);
  }
  process.exit(0);
}

main();
