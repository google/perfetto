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

// Generation of reference from protos

'use strict';

const fs = require('fs');
const argv = require('yargs').argv

// Removes \n due to 80col wrapping and preserves only end-of-sentence line
// breaks.
// TODO dedupe, this is copied from the other gen_proto file.
function singleLineComment(comment) {
  comment = comment || '';
  comment = comment.trim();
  comment = comment.replace(/\.\n/g, '<br>');
  comment = comment.replace(/\n/g, ' ');
  return comment;
}

function trimQuotes(s) {
  if (s === undefined) {
    return s;
  }
  const regex = /\"(.*)"/;
  let m = regex.exec(s);
  if (m === null) {
    return null;
  }
  return m[1]
}

function parseTablesInCppFile(filePath) {
  const hdr = fs.readFileSync(filePath, 'UTF8');
  const regex = /^\s*F\(([\s\S]*?)\),\s*\\/mg;
  let match;
  let table = [];
  while ((match = regex.exec(hdr)) !== null) {
    let def = match[1];
    let s = def.split(',').map(s => s.trim());
    table.push({
      name: s[0],
      cardinality: s[1],
      type: s[2],
      scope: s[3],
      comment: s[4] === undefined ? undefined :
                                    s[4].split('\n').map(trimQuotes).join(' '),
    });
  }
  return table;
}


function tableToMarkdown(table) {
  let md = `# Trace Processor Stats\n\n`;
  md += `<table><thead><tr><td>Name</td><td>Cardinality</td><td>Type</td>
  <td>Scope</td><td>Description</td></tr></thead>\n`;
  for (const col of table) {
    md += `<tr id="${col.name}"><td>${col.name}</td>
    <td>${col.cardinality}</td><td>${col.type}</td><td>${col.scope}</td>
    <td>${singleLineComment(col.comment)} </td></tr>\n`
  }
  md += '</table>\n\n';
  return md;
}

function main() {
  const inFile = argv['i'];
  const outFile = argv['o'];
  if (!inFile) {
    console.error('Usage: -i hdr1.h -i hdr2.h -[-o out.md]');
    process.exit(1);
  }

  // Can be either a string (-i single) or an array (-i one -i two).
  const inFiles = (inFile instanceof Array) ? inFile : [inFile];

  const table = Array.prototype.concat(...inFiles.map(parseTablesInCppFile));
  const md = tableToMarkdown(table);
  if (outFile) {
    fs.writeFileSync(outFile, md);
  } else {
    console.log(md);
  }
  process.exit(0);
}

main();
