// Copyright (C) 2021 The Android Open Source Project
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

const path = require('path');
const http = require('http');
const childProcess = require('child_process');

module.exports = async function() {
  // Start the local HTTP server.
  const ROOT_DIR = path.dirname(path.dirname(__dirname));
  const node = path.join(ROOT_DIR, 'ui', 'node');
  const args = [
    path.join(ROOT_DIR, 'ui', 'build.js'),
    '--serve',
    '--no-build',
    '--out=.',
  ];
  const spwOpts = {stdio: ['ignore', 'inherit', 'inherit']};
  const srvProc = childProcess.spawn(node, args, spwOpts);
  global.__DEV_SERVER__ = srvProc;

  // Wait for the HTTP server to be ready.
  let attempts = 10;
  for (; attempts > 0; attempts--) {
    await new Promise((r) => setTimeout(r, 1000));
    try {
      await new Promise((resolve, reject) => {
        const req = http.request('http://127.0.0.1:10000/frontend_bundle.js');
        req.end();
        req.on('error', (err) => reject(err));
        req.on('finish', () => resolve());
      });
      break;
    } catch (err) {
      console.error('Waiting for HTTP server to come up', err.message);
    }
  }
  if (attempts === 0) {
    throw new Error('HTTP server didn\'t come up');
  }
  if (srvProc.exitCode !== null) {
    throw new Error(
        `The dev server unexpectedly exited, code=${srvProc.exitCode}`);
  }
};
