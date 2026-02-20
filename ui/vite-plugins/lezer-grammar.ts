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
import {existsSync, mkdirSync, readFileSync, statSync} from 'fs';

const UI_DIR = resolve(__dirname, '..');

// Plugin to compile Lezer grammar files on-demand
export function lezerGrammarPlugin(): Plugin {
  const cacheDir = resolve(UI_DIR, 'node_modules/.vite-lezer-cache');

  // Check if cached output is valid (exists and newer than source)
  function isCacheValid(grammarPath: string, cachedPath: string): boolean {
    if (!existsSync(cachedPath)) return false;
    const cacheMtime = statSync(cachedPath).mtimeMs;
    const sourceMtime = statSync(grammarPath).mtimeMs;
    return cacheMtime > sourceMtime;
  }

  return {
    name: 'lezer-grammar',
    enforce: 'pre',

    resolveId(source, importer) {
      if (source.endsWith('.grammar') && importer) {
        // Resolve to virtual module for compilation
        const importerDir = resolve(importer, '..');
        const grammarPath = resolve(importerDir, source);
        return `\0virtual:lezer:${grammarPath}`;
      }
      return null;
    },

    load(id) {
      if (id.startsWith('\0virtual:lezer:')) {
        const grammarPath = id.replace('\0virtual:lezer:', '');

        if (!existsSync(grammarPath)) {
          throw new Error(`Grammar file not found: ${grammarPath}`);
        }

        // Create cache key from grammar path
        const cacheKey = grammarPath.replace(/[/\\]/g, '_').replace(/:/g, '');
        const cachedJs = resolve(cacheDir, `${cacheKey}.js`);

        // Check cache with mtime validation
        if (isCacheValid(grammarPath, cachedJs)) {
          return readFileSync(cachedJs, 'utf-8');
        }

        // Compile using lezer-generator
        if (!existsSync(cacheDir)) {
          mkdirSync(cacheDir, {recursive: true});
        }

        const lezerPath = resolve(UI_DIR, 'node_modules/.bin/lezer-generator');

        try {
          execSync(`${lezerPath} ${grammarPath} -o ${cachedJs}`, {
            stdio: 'pipe',
          });
          return readFileSync(cachedJs, 'utf-8');
        } catch (e) {
          console.error('Failed to compile grammar:', e);
          throw e;
        }
      }
      return null;
    },
  };
}
