// Copyright (C) 2026 The Android Open Source Project
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

// Build-time generators for the auto-generated reference pages. These used to
// be standalone CLIs (gen_stats_reference.js, gen_proto_reference.js,
// gen_sql_tables_reference.js) spawned by GN actions; they are now plain
// functions returning markdown, called by build.mjs in-process.

import fs from "node:fs";
import path from "node:path";
import protobufjs from "protobufjs";
import { ROOT_DIR } from "./render.mjs";

// ---------------------------------------------------------------------------
// Shared comment helpers.
// ---------------------------------------------------------------------------

// Removes \n due to 80col wrapping and preserves only end-of-sentence line
// breaks.
function singleLineCommentPlain(comment) {
  comment = comment || "";
  comment = comment.trim();
  comment = comment.replace(/\.\n/g, "<br>");
  comment = comment.replace(/\n/g, " ");
  return comment;
}

// ---------------------------------------------------------------------------
// Trace Processor stats (src/trace_processor/storage/stats.h).
// ---------------------------------------------------------------------------

function trimQuotes(s) {
  if (s === undefined) {
    return s;
  }
  const regex = /\"(.*)"/;
  let m = regex.exec(s);
  if (m === null) {
    return null;
  }
  return m[1];
}

function parseTablesInCppFile(filePath) {
  const hdr = fs.readFileSync(filePath, "utf8");
  const regex = /^\s*F\(([\s\S]*?)\),\s*\\/gm;
  let match;
  let table = [];
  while ((match = regex.exec(hdr)) !== null) {
    let def = match[1];
    let s = def.split(",").map((s) => s.trim());
    table.push({
      name: s[0],
      cardinality: s[1],
      type: s[2],
      scope: s[3],
      comment:
        s[4] === undefined
          ? undefined
          : s[4].split("\n").map(trimQuotes).join(" "),
    });
  }
  return table;
}

export function genStatsMd(headerPaths) {
  const table = [].concat(...headerPaths.map(parseTablesInCppFile));
  let md = `# Trace Processor Stats\n\n`;
  md += `<table><thead><tr><td>Name</td><td>Cardinality</td><td>Type</td>
  <td>Scope</td><td>Description</td></tr></thead>\n`;
  for (const col of table) {
    md += `<tr id="${col.name}"><td>${col.name}</td>
    <td>${col.cardinality}</td><td>${col.type}</td><td>${col.scope}</td>
    <td>${singleLineCommentPlain(col.comment)} </td></tr>\n`;
  }
  md += "</table>\n\n";
  return md;
}

// ---------------------------------------------------------------------------
// Proto reference (protos/**/*.proto via protobufjs, no protoc needed).
// ---------------------------------------------------------------------------

// This function is used to escape:
// - The message-level comment, which becomes a full paragraph.
// - The per-field comments, rendered as as table.
function escapeCommentCommon(comment) {
  comment = comment || "";

  // Remove Next id: NN lines.
  comment = comment.replace(/(\n)?^\s*next.*\bid:.*$/gim, "");

  // Hide our little dirty secrets.
  comment = comment.replace(/(\n)?^\s*TODO\(\w+\):.*$/gim, "");

  // Turn |variable| references into `variable`.
  comment = comment.replace(/[|](\w+?)[|]/g, "`$1`");
  return comment;
}

function singleLineProtoComment(comment) {
  comment = escapeCommentCommon(comment);
  comment = comment.trim();
  comment = comment.replace(/([.:?!])\n/g, "$1<br>");
  comment = comment.replace(/\n/g, " ");
  return comment;
}

function getFullName(pType) {
  let cur = pType;
  let name = pType.name;
  while (cur && cur.parent != cur && cur.parent instanceof protobufjs.Type) {
    name = `${cur.parent.name}.${name}`;
    cur = cur.parent;
  }
  return name;
}

// `visited` used to be a module-level global. It must be per-invocation: we
// generate two proto pages in the same process now, and a shared set would make
// the second page silently omit every type the first one already emitted.
function genType(pType, depth, visited) {
  depth = depth || 0;
  const fullName = getFullName(pType);
  if (visited.has(fullName)) return "";
  visited.add(fullName);

  const heading = "#" + "#".repeat(Math.min(depth, 2));
  const anchor = depth > 0 ? `{#${fullName}} ` : "";
  let md = `${heading} ${anchor}${fullName}`;
  md += "\n";
  const fileName = path.basename(pType.filename);
  const relPath = path.relative(ROOT_DIR, pType.filename);

  md += escapeCommentCommon(pType.comment);
  md += `\n\nDefined in [${fileName}](/${relPath})\n\n`;

  const subTypes = [];

  if (pType instanceof protobufjs.Enum) {
    md += "#### Enum values:\n";
    md += "Name | Value | Description\n";
    md += "---- | ----- | -----------\n";
    for (const enumName of Object.keys(pType.values)) {
      const enumVal = pType.values[enumName];
      const comment = singleLineProtoComment(pType.comments[enumName]);
      md += `${enumName} | ${enumVal} | ${comment}\n`;
    }
  } else {
    md += "#### Fields:\n";
    md += "Field | Type | Description\n";
    md += "----- | ---- | -----------\n";

    for (const fieldName in pType.fields) {
      const field = pType.fields[fieldName];
      let type = field.type;
      if (field.repeated) {
        type = `${type}[]`;
      }
      if (field.resolvedType) {
        // The TraceConfig proto is linked from the TracePacket reference.
        // Instead of recursing and generating the TraceConfig types all over
        // again, just link to the dedicated TraceConfig reference page.
        if (getFullName(field.resolvedType) === "TraceConfig") {
          type = `[${type}](/docs/reference/trace-config-proto.autogen)`;
        } else {
          subTypes.push(field.resolvedType);
          type = `[${type}](#${getFullName(field.resolvedType)})`;
        }
      }
      md += `${fieldName} | ${type} | ${singleLineProtoComment(field.comment)}\n`;
    }
  }
  md += "\n\n\n\n";

  for (const subType of subTypes) md += genType(subType, depth + 1, visited);

  return md;
}

export function genProtoMd(protoPath, messageName) {
  const parser = new protobufjs.Root();
  parser.resolvePath = (_, target) => {
    // The root proto is passed as an absolute path; imports inside it are
    // relative to the project root (e.g. protos/perfetto/config/...).
    if (target === protoPath) return protoPath;
    return path.join(ROOT_DIR, target);
  };

  const cfg = parser.loadSync(protoPath, {
    alternateCommentMode: true,
    keepCase: true,
  });
  cfg.resolveAll();
  const rootType = cfg.lookup(messageName);
  return genType(rootType, 0, new Set());
}

// ---------------------------------------------------------------------------
// PerfettoSQL prelude tables (from tools/gen_tp_table_docs.py's JSON).
// ---------------------------------------------------------------------------

function singleLineTableComment(comment) {
  comment = comment || "";
  comment = comment.trim();
  comment = comment.replaceAll("|", "\\|");
  comment = comment.replace(/\.\n/g, "<br>");
  comment = comment.replace(/\n/g, " ");
  return comment;
}

function genLink(table) {
  return `[${table.name}](#${table.name})`;
}

function tableToMarkdown(table) {
  let md = `### ${table.name}\n\n`;
  if (table.parent) {
    md += `_Extends ${genLink(table.parent)}_\n\n`;
  }
  md += table.comment + "\n\n";
  md += "Column | Type | Description\n";
  md += "------ | ---- | -----------\n";

  let curTable = table;
  while (curTable) {
    if (curTable != table) {
      md += `||_Columns inherited from_ ${genLink(curTable)}\n`;
    }
    for (const col of Object.values(curTable.cols)) {
      const type = col.type + (col.optional ? "<br>`optional`" : "");
      let description = col.comment;
      if (col.joinTable) {
        description +=
          `\nJoinable with ` +
          `[${col.joinTable}.${col.joinCol}](#${col.joinTable})`;
      }
      md += `${col.name} | ${type} | ${singleLineTableComment(description)}\n`;
    }
    curTable = curTable.parent;
  }
  md += "\n\n";
  return md;
}

export function genSqlTablesMd(jsonPaths) {
  const jsonTables = [].concat(
    ...jsonPaths.map((p) => JSON.parse(fs.readFileSync(p, "utf8"))),
  );

  // Resolve parents.
  const tablesIndex = {}; // 'TP_SCHED_SLICE_TABLE_DEF' -> table
  const tablesByGroup = {}; // 'profilers' => [table1, table2]
  const tablesCppName = {}; // 'StackProfileMappingTable' => table
  const tablesByName = {}; // 'profile_mapping' => table
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
    const keys = { Tracks: "1", Events: "2", Misc: "z" };
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
  let graph = "## Tables diagram\n";
  const mkLabel = (table) => `${table.defMacro}["${table.name}"]`;
  for (const tableGroup of tableGroups) {
    let graphEdges = "";
    let graphLinks = "";
    graph += `#### ${tableGroup} tables\n`;
    graph += "```mermaid\ngraph TD\n";
    graph += `  subgraph ${tableGroup}\n`;
    for (const table of tablesByGroup[tableGroup]) {
      graph += `  ${mkLabel(table)}\n`;
      graphLinks += `  click ${table.defMacro} "#${table.name}"\n`;
      if (table.parent) {
        graphEdges += ` ${mkLabel(table)} --> ${mkLabel(table.parent)}\n`;
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
        if (!refTable) continue;
        graphEdges += `  ${mkLabel(table)} -. ${col.name} .-> ${mkLabel(refTable)}\n`;
        graphLinks += `  click ${refTable.defMacro} "#${refTable.name}"\n`;
      }
    }
    graph += `  end\n`;
    graph += graphEdges;
    graph += graphLinks;
    graph += "\n```\n";
  }

  let md = "# PerfettoSQL Prelude\n" + graph;
  for (const tableGroup of tableGroups) {
    md += `## ${tableGroup}\n`;
    for (const table of tablesByGroup[tableGroup]) {
      md += tableToMarkdown(table);
    }
  }
  return md;
}
