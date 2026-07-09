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

// Loads the syntaqlite WASM runtime + the PerfettoSQL dialect once, then hands
// out a fresh WASM session (Engine) per call. Sessions are independent — each
// hosts its own LSP server lifecycle, which matters because a server accepts
// exactly one `initialize`: reusing one session across traces would reject the
// second trace's handshake. If the WASM fails to load, resolves to undefined
// and the feature silently disappears.

import {Engine, type EmscriptenModule} from 'syntaqlite';
import {assetSrc} from '../../base/assets';

interface Base {
  readonly runtime: EmscriptenModule;
  readonly dialectPtr: number;
}

let basePromise: Promise<Base | undefined> | undefined;

async function loadBase(): Promise<Base | undefined> {
  try {
    const bootstrap = new Engine({
      runtimeJsPath: assetSrc('assets/syntaqlite-runtime.js'),
      runtimeWasmPath: assetSrc('assets/syntaqlite-runtime.wasm'),
    });
    await bootstrap.load();
    const binding = await bootstrap.loadDialectFromUrl(
      assetSrc('assets/syntaqlite-perfetto.wasm'),
      'syntaqlite_perfetto_dialect_template',
    );
    if (!bootstrap.lspSupported) {
      console.warn(
        'syntaqlite runtime has no LSP entry point; ' +
          'SQL editor intelligence disabled',
      );
      return undefined;
    }
    const runtime = bootstrap.runtimeModule;
    if (runtime === undefined) return undefined;
    // The runtime module and the loaded dialect are shared across sessions;
    // only the bootstrap session itself is freed here.
    bootstrap.dispose();
    return {runtime, dialectPtr: binding.ptr};
  } catch (e) {
    console.warn(
      'syntaqlite engine failed to load; SQL editor intelligence disabled',
      e,
    );
    return undefined;
  }
}

// A fresh session on the shared runtime. Callers own its lifetime and must
// dispose() it when done (e.g. on trace unload).
export async function createSqlEngine(): Promise<Engine | undefined> {
  if (basePromise === undefined) {
    basePromise = loadBase();
  }
  const base = await basePromise;
  if (base === undefined) return undefined;
  try {
    const engine = new Engine({runtime: base.runtime});
    await engine.load();
    engine.setDialectPointer(base.dialectPtr);
    return engine;
  } catch (e) {
    console.warn('failed to create syntaqlite session', e);
    return undefined;
  }
}
