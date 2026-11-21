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

// Applies the UI's lint rules to products/*/ui, which lives outside ui/.
const path = require('node:path');
const uiConfig = require('../ui/eslint.config.js');

module.exports = uiConfig
  .filter((c) => c.ignores === undefined)
  .map((c) => {
    if (c.files === undefined) return c;
    return {
      ...c,
      files: ['*/ui/**/*.ts'],
      languageOptions: {
        ...c.languageOptions,
        parserOptions: {
          ...c.languageOptions?.parserOptions,
          project: path.resolve(__dirname, '../ui/tsconfig.json'),
        },
      },
    };
  });
