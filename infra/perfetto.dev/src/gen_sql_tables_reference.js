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

// Returns an object describing the table as follows:
// { name: 'HeapGraphObjectTable',
//   cols: [ {name: 'upid',            type: 'uint32_t', optional: false },
//           {name: 'graph_sample_ts', type: 'int64_t',  optional: false },
function parseTableDef(tableDefName, tableDef) {
  const tableDesc = {
    name: '',                // The SQL table name, e.g. stack_profile_mapping.
    cppClassName: '',        // e.g., StackProfileMappingTable.
    defMacro: tableDefName,  // e.g., PERFETTO_TP_STACK_PROFILE_MAPPING_DEF.
    comment: '',
    parent: undefined,   // Will be filled afterwards in the resolution phase.
    parentDefName: '',   // e.g., PERFETTO_TP_STACK_PROFILE_MAPPING_DEF.
    tablegroup: 'Misc',  // From @tablegroup in comments.
    cols: {},
  };
  const getOrCreateColumn = (name) => {
    if (name in tableDesc.cols)
      return tableDesc.cols[name];
    tableDesc.cols[name] = {
      name: name,
      type: '',
      comment: '',
      optional: false,
      refTableCppName: undefined,
      joinTable: undefined,
      joinCol: undefined,
    };
    return tableDesc.cols[name];
  };

  // Reserve the id and type columns so they appear first in the column list
  // They will only be kept in case this is a root table - otherwise they will
  // be deleted below..
  const id = getOrCreateColumn('id');
  const type = getOrCreateColumn('type');

  let lastColumn = undefined;
  for (const line of tableDef.split('\n')) {
    if (line.startsWith('#define'))
      continue;  // Skip the first line.
    let m;
    if (line.startsWith('//')) {
      let comm = line.replace(/^\s*\/\/\s*/, '');
      if (m = comm.match(/@tablegroup (.*)/)) {
        tableDesc.tablegroup = m[1];
        continue;
      }
      if (m = comm.match(/@name (\w+)/)) {
        tableDesc.name = m[1];
        continue;
      }
      if (m = comm.match(/@param\s+([^ ]+)\s*({\w+})?\s*(.*)/)) {
        lastColumn = getOrCreateColumn(/*name=*/ m[1]);
        lastColumn.type = (m[2] || '').replace(/(^{)|(}$)/g, '');
        lastColumn.comment = m[3];
        continue;
      }
      if (lastColumn === undefined) {
        tableDesc.comment += `${comm}\n`;
      } else {
        lastColumn.comment = `${lastColumn.comment}\n${comm}`;
      }
      continue;
    }
    if (m = line.match(/^\s*NAME\((\w+)\s*,\s*"(\w+)"/)) {
      tableDesc.cppClassName = m[1];
      if (tableDesc.name === '') {
        tableDesc.name = m[2];  // Set only if not overridden by @name.
      }
      continue;
    }
    if (m = line.match(/(PERFETTO_TP_ROOT_TABLE|PARENT)\((\w+)/)) {
      if (m[1] === 'PARENT') {
        tableDesc.parentDefName = m[2];
      }
      continue;
    }
    if (m = line.match(/^\s*C\(([^,]+)\s*,\s*(\w+)/)) {
      const col = getOrCreateColumn(/*name=*/ m[2]);
      col.type = m[1];
      if (m = col.type.match(/Optional<(.*)>/)) {
        col.type = m[1];
        col.optional = true;
      }
      if (col.type === 'StringPool::Id') {
        col.type = 'string';
      }
      const sep = col.type.indexOf('::');
      if (sep > 0) {
        col.refTableCppName = col.type.substr(0, sep);
      }
      continue;
    }
    throw new Error(`Cannot parse line "${line}" from ${tableDefName}`);
  }

  if (tableDesc.parentDefName === '') {
    id.type = `${tableDesc.cppClassName}::Id`;
    type.type = 'string';
  } else {
    delete tableDesc.cols['id'];
    delete tableDesc.cols['type'];
  }

  // Process {@joinable xxx} annotations.
  const regex = /\s?\{@joinable\s*(\w+)\.(\w+)\s*\}/;
  for (const col of Object.values(tableDesc.cols)) {
    const m = col.comment.match(regex)
    if (m) {
      col.joinTable = m[1];
      col.joinCol = m[2];
      col.comment = col.comment.replace(regex, '');
    }
  }
  return tableDesc;
}


function parseTablesInCppFile(filePath) {
  const hdr = fs.readFileSync(filePath, 'UTF8');
  const regex = /^\s*PERFETTO_TP_TABLE\((\w+)\)/mg;
  let match = regex.exec(hdr);
  const tables = [];
  while (match != null) {
    const tableDefName = match[1];
    match = regex.exec(hdr);

    // Now let's extract the table definition, that looks like this:
    // // Some
    // // Multiline
    // // Comment
    // #define PERFETTO_TP_STACK_PROFILE_FRAME_DEF(NAME, PARENT, C) \
    // NAME(StackProfileFrameTable, "stack_profile_frame")        \
    // PERFETTO_TP_ROOT_TABLE(PARENT, C)                          \
    // C(StringPool::Id, name)                                    \
    // C(StackProfileMappingTable::Id, mapping)                   \
    // C(int64_t, rel_pc)                                         \
    // C(base::Optional<uint32_t>, symbol_set_id)
    //
    // Where PERFETTO_TP_STACK_PROFILE_FRAME_DEF is |tableDefName|.
    let pattern = `(^[ ]*//.*\n)*`;
    pattern += `^\s*#define\\s+${tableDefName}\\s*\\(`;
    pattern += `(.*\\\\\\s*\n)+`;
    pattern += `.+`;
    const r = new RegExp(pattern, 'mi');
    const tabMatch = r.exec(hdr);
    if (!tabMatch) {
      console.error(`could not find table ${tableDefName}`);
      continue;
    }
    tables.push(parseTableDef(tableDefName, tabMatch[0]));
  }
  return tables;
}

function parseTablesInJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'UTF8'));
}

function overrideCppTablesWithJsonTables(cpp, json) {
  const out = [];
  const jsonAdded = new Set();
  for (const table of json) {
    out.push(table);
    jsonAdded.add(table.name);
  }
  for (const table of cpp) {
    if (!jsonAdded.has(table.name)) {
      out.push(table);
    }
  }
  return out;
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
  const inFile = argv['i'];
  const outFile = argv['o'];
  const jsonFile = argv['j'];
  if (!inFile) {
    console.error('Usage: -i hdr1.h -i hdr2.h -j tbls.json -[-o out.md]');
    process.exit(1);
  }

  // Can be either a string (-i single) or an array (-i one -i two).
  const inFiles = (inFile instanceof Array) ? inFile : [inFile];
  const cppTables =
      Array.prototype.concat(...inFiles.map(parseTablesInCppFile));

  // Can be either a string (-j single) or an array (-j one -j two).
  const jsonFiles = (jsonFile instanceof Array) ? jsonFile : [jsonFile];
  const jsonTables =
      Array.prototype.concat(...jsonFiles.map(parseTablesInJson));
  const tables = overrideCppTablesWithJsonTables(cppTables, jsonTables)

  // Resolve parents.
  const tablesIndex = {};    // 'TP_SCHED_SLICE_TABLE_DEF' -> table
  const tablesByGroup = {};  // 'profilers' => [table1, table2]
  const tablesCppName = {};  // 'StackProfileMappingTable' => table
  const tablesByName = {};   // 'profile_mapping' => table
  for (const table of tables) {
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

  for (const table of tables) {
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

  let md = graph;
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
