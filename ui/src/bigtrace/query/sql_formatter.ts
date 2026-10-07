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

import {Engine} from 'syntaqlite';
import {assetSrc} from '../../base/assets';

// Lazy module singleton — the wasm assets are only fetched on first use.
let enginePromise: Promise<Engine> | undefined;

function getFormatterEngine(): Promise<Engine> {
  if (enginePromise === undefined) {
    const engine = new Engine({
      runtimeJsPath: assetSrc('assets/syntaqlite-runtime.js'),
      runtimeWasmPath: assetSrc('assets/syntaqlite-runtime.wasm'),
    });
    enginePromise = (async () => {
      await engine.load();
      const binding = await engine.loadDialectFromUrl(
        assetSrc('assets/syntaqlite-perfetto.wasm'),
        'syntaqlite_perfetto_dialect_template',
      );
      engine.setDialectPointer(binding.ptr);
      return engine;
    })();
  }
  return enginePromise;
}

// Format PerfettoSQL with the same options the main Query page uses.
// Returns undefined (and leaves the caller's text untouched) on failure.
export async function formatPerfettoSql(
  text: string,
): Promise<string | undefined> {
  try {
    const engine = await getFormatterEngine();
    return engine.format(text, {
      lineWidth: 80,
      indentWidth: 2,
      keywordCase: 'upper',
      semicolons: true,
    });
  } catch (e) {
    console.error('SQL formatting failed', e);
  }
  return undefined;
}
