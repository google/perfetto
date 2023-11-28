// Copyright (C) 2023 The Android Open Source Project
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

// Generation of UI API references

'use strict';

const fs = require('fs');
const path = require('path');
const argv = require('yargs').argv

const PROJECT_ROOT =
    path.dirname(path.dirname(path.dirname(path.dirname(__filename))));

function main() {
  const inputPath = argv['i'];
  const outputPath = argv['o'];
  if (!inputPath) {
    console.error('Usage: -i ui/src/public/index.ts [-o out.md]');
    process.exit(1);
  }

  const text = fs.readFileSync(inputPath, 'UTF8');

  const generatedMd = '```\n' + text + '```\n';

  if (outputPath) {
    fs.writeFileSync(outputPath, generatedMd);
  } else {
    console.log(generatedMd);
  }
  process.exit(0);
}

main();
