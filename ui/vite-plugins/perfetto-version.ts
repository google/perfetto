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

import {Plugin} from 'vite';
import {resolve} from 'path';
import {execSync} from 'child_process';

const ROOT_DIR = resolve(__dirname, '../..');

// Plugin to generate perfetto_version module on-demand
export function perfettoVersionPlugin(): Plugin {
  let cachedCode: string | null = null;

  function generateVersionCode(): string {
    const scriptPath = resolve(ROOT_DIR, 'tools/write_version_header.py');

    try {
      const output = execSync(`python3 ${scriptPath} --json`, {
        encoding: 'utf-8',
        stdio: ['pipe', 'pipe', 'pipe'],
      }).trim();

      const {version, scm_revision} = JSON.parse(output);

      return [
        `export const VERSION = "${version}";`,
        `export const SCM_REVISION = "${scm_revision}";`,
      ].join('\n');
    } catch (e) {
      console.error('Failed to generate version:', e);
      // Fallback for when git/python isn't available
      return [
        'export const VERSION = "unknown";',
        'export const SCM_REVISION = "unknown";',
      ].join('\n');
    }
  }

  return {
    name: 'perfetto-version',
    enforce: 'pre',

    resolveId(source, importer) {
      if (source === '../gen/perfetto_version' && importer) {
        return '\0virtual:perfetto_version';
      }
      return null;
    },

    load(id) {
      if (id === '\0virtual:perfetto_version') {
        // Cache the result for the duration of the build
        if (cachedCode === null) {
          cachedCode = generateVersionCode();
        }
        return cachedCode;
      }
      return null;
    },
  };
}
