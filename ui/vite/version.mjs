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

import {execFileSync} from 'node:child_process';
import {makeSynthModulePlugin} from './utils.mjs';

// Exposes VERSION and SCM_REVISION via |virtualModule|. |scriptPath| points
// at tools/write_version_header.py (invoked with --json). Replaces the
// on-disk perfetto_version.ts that build.mjs used to generate.
export function pluginPerfettoVersion({virtualModule, scriptPath}) {
  const generate = () => {
    const out = execFileSync('python3', [scriptPath, '--json'], {
      encoding: 'utf8',
    });
    const {version, sha1} = JSON.parse(out);
    return (
      `export const VERSION = ${JSON.stringify(version)};\n` +
      `export const SCM_REVISION = ${JSON.stringify(sha1)};\n`
    );
  };
  return makeSynthModulePlugin({
    name: 'perfetto:version',
    modules: {[virtualModule]: generate},
  });
}
