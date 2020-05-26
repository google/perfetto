// Copyright (C) 2018 The Android Open Source Project
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

/**
 * Configuration file for lite-server. Contains configuration for auto rerunning
 * ninja on file change.
 */
'use strict';

const { spawn } = require('child_process');
const path = require('path');

// Print without added new line.
const print = data => process.stdout.write(data);
const printErr = data => process.stderr.write(data);

const ninjaOutDir = process.env.OUT_DIR;
const uiOutDir = path.join(ninjaOutDir, 'ui');
const perfettoRoot = process.env.ROOT_DIR;
const ninjaPath = path.join(perfettoRoot, 'tools', 'ninja');
let ninjaRunning = false;

function rebasePath(relative_path) {
  return path.join(perfettoRoot, relative_path);
}

module.exports = function(bs) {
  return {
    files: [
      {
        match: [
          "ui/**",
          "src/trace_processor/**",
          "protos/**",
        ].map(rebasePath),
        fn: function(event, file) {
          console.log(`Change detected on ${file}`);
          if (ninjaRunning) {
            console.log("Already have a ninja build running. Doing nothing.");
            return;
          }

          ninjaRunning = true;

          console.log(`Executing: ninja -C ${ninjaOutDir} ui`);
          const ninja = spawn(ninjaPath, ['-C', ninjaOutDir, 'ui']);
          ninja.stdout.on('data', data => print(data.toString()));
          ninja.stderr.on('data', data => printErr(data.toString()));

          // We can be smarter and load just the file we need. Need to
          // resolve to the dist/location of the file in that case.
          // For now, we're reloading the whole page.
          ninja.on('exit', () => {
            ninjaRunning = false;
            bs.reload();
          });
        },
        options: {
          ignored: [
            "ui/dist/",
            "ui/.git/",
            "ui/node_modules/",
          ].map(rebasePath),
          ignoreInitial: true,
        }
      }
    ],
    server: {
      baseDir: uiOutDir,
    },
  };
};
