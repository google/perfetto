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

// Vitest setup file. Mirrors what the old jest config did:
//   - jest-canvas-mock: stubs HTMLCanvasElement.getContext
//   - jest-localstorage-mock: in-memory localStorage / sessionStorage
//   - structuredClone polyfill (jsdom still doesn't ship it)
//   - `jest` -> `vi` alias so existing *_unittest.ts files keep working

import {vi} from 'vitest';
import {createRequire} from 'node:module';
import { VitestUtils } from 'vitest';

// Existing tests use `jest.fn()`, `jest.spyOn()`, etc. Vitest's `vi` is API-
// compatible, so expose it as a global. This MUST happen before the
// jest-canvas-mock / jest-localstorage-mock requires below, which themselves
// call `jest.fn()` at module-load time.
// eslint-disable-next-line @typescript-eslint/no-explicit-any
(globalThis as unknown as {jest: VitestUtils}).jest = vi;

// Use require (not import) so these load *after* the global above is set.
// Static `import` is hoisted to the top of the module; dynamic `import()` is
// blocked by these packages' lack of ESM exports.
const require = createRequire(import.meta.url);
require('jest-canvas-mock');
require('jest-localstorage-mock');

// jsdom doesn't expose structuredClone yet. Node's global one is fine.
if (typeof globalThis.structuredClone === 'undefined') {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  (globalThis as any).structuredClone = structuredClone;
}
