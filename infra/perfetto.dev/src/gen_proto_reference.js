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

const protobufjs = require('protobufjs');
const fs = require('fs');
const path = require('path');
const argv = require('yargs').argv

const PROJECT_ROOT =
    path.dirname(path.dirname(path.dirname(path.dirname(__filename))));

const visited = {};

// This function is used to escape:
// - The message-level comment, which becomes a full paragraph.
// - The per-field comments, rendered as as table.
function escapeCommentCommon(comment) {
  comment = comment || '';

  // Remove Next id: NN lines.
  comment = comment.replace(/(\n)?^\s*next.*\bid:.*$/img, '');

  // Hide our little dirty secrets.
  comment = comment.replace(/(\n)?^\s*TODO\(\w+\):.*$/img, '');

  // Turn |variable| references into `variable`.
  comment = comment.replace(/[|](\w+?)[|]/g, '`$1`');
  return comment;
}

// This is used to escape only the per-field comments.
// Removes \n due to 80col wrapping and preserves only end-of-sentence line
// breaks.
function singleLineComment(comment) {
  comment = escapeCommentCommon(comment);
  comment = comment.trim();
  comment = comment.replace(/([.:?!])\n/g, '$1<br>');
  comment = comment.replace(/\n/g, ' ');
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

function genType(pType, depth) {
  depth = depth || 0;
  console.assert(pType instanceof protobufjs.ReflectionObject);
  const fullName = getFullName(pType);
  if (fullName in visited)
    return '';
  visited[fullName] = true;

  const heading = '#' +
      '#'.repeat(Math.min(depth, 2));
  const anchor = depth > 0 ? `{#${fullName}} ` : '';
  let md = `${heading} ${anchor}${fullName}`;
  md += '\n';
  const fileName = path.basename(pType.filename);
  const relPath = path.relative(PROJECT_ROOT, pType.filename);

  md += escapeCommentCommon(pType.comment);
  md += `\n\nDefined in [${fileName}](/${relPath})\n\n`;

  const subTypes = [];

  if (pType instanceof protobufjs.Enum) {
    md += '#### Enum values:\n';
    md += 'Name | Value | Description\n';
    md += '---- | ----- | -----------\n';
    for (const enumName of Object.keys(pType.values)) {
      const enumVal = pType.values[enumName];
      const comment = singleLineComment(pType.comments[enumName]);
      md += `${enumName} | ${enumVal} | ${comment}\n`
    }
  } else {
    md += '#### Fields:\n';
    md += 'Field | Type | Description\n';
    md += '----- | ---- | -----------\n';

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
        if (getFullName(field.resolvedType) === 'TraceConfig') {
          type = `[${type}](/docs/reference/trace-config-proto.autogen)`;
        } else {
          subTypes.push(field.resolvedType);
          type = `[${type}](#${getFullName(field.resolvedType)})`;
        }
      }
      md += `${fieldName} | ${type} | ${singleLineComment(field.comment)}\n`
    }
  }
  md += '\n\n\n\n';

  for (const subType of subTypes)
    md += genType(subType, depth + 1);

  return md;
}


function main() {
  const inProtoFile = argv['i'];
  const protoName = argv['p'];
  const outFile = argv['o'];
  if (!inProtoFile || !protoName) {
    console.error('Usage: -i input.proto -p protos.RootType [-o out.md]');
    process.exit(1);
  }

  const parser = new protobufjs.Root();
  parser.resolvePath = (_, target) => {
    if (target == inProtoFile) {
      // The root proto file passed from the cmdline will be relative to the
      // root_build_dir (out/xxx) (e.g.: ../../protos/config)
      return inProtoFile;
    }
    // All the other imports, instead, will be relative to the project root
    // (e.g. protos/config/...)
    return path.join(PROJECT_ROOT, target);
  };

  const cfg = parser.loadSync(
      inProtoFile, {alternateCommentMode: true, keepCase: true});
  cfg.resolveAll();
  const traceConfig = cfg.lookup(protoName);
  const generatedMd = genType(traceConfig);
  if (outFile) {
    fs.writeFileSync(outFile, generatedMd);
  } else {
    console.log(generatedMd);
  }
  process.exit(0);
}

main();
