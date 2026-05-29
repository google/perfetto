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

// Pulls in ambient declarations for `test`, `expect`, `describe`, `vi`, etc.
// vitest.config.mjs has `globals: true` so these are available without
// importing from 'vitest' in every *_unittest.ts file. Note: type aliases
// like `Mock` and `Mocked` are NOT injected as globals — those must be
// imported explicitly: `import type {Mock, Mocked} from 'vitest';`
// / <reference types="vitest/globals" />
