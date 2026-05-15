// Copyright (C) 2019 The Android Open Source Project
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

module.exports = {
  transform: {
    // Transpile TS on the fly with @swc/jest. Vite owns the production build,
    // so there are no pre-emitted .js files for jest to consume any more.
    '^.+\\.tsx?$': ['@swc/jest', {
      jsc: {
        parser: {syntax: 'typescript', tsx: false, decorators: false},
        target: 'es2022',
      },
    }],
    // Some first-party generated files (e.g. lezer .grammar.js) are ESM —
    // run them through swc so jest's CJS runtime can require them.
    '^.+\\.jsx?$': ['@swc/jest', {
      jsc: {
        parser: {syntax: 'ecmascript', jsx: false},
        target: 'es2022',
      },
      module: {type: 'commonjs'},
    }],
  },
  testRegex: '_(unittest|jsdomtest)[.]ts$',
  testEnvironment: __dirname + '/JestJsdomEnv.js',
  setupFiles: [
    'jest-canvas-mock',
    'jest-localstorage-mock',
  ],
  moduleNameMapper: {
    '^syntaqlite$': __dirname + '/syntaqlite_mock.js',
    // Side-effect imports of CSS/SCSS become no-ops in tests.
    '\\.(css|scss)$': __dirname + '/style_mock.js',
    // Some TS files use ESM-style explicit ".js" extensions on relative
    // imports of sibling TS sources (e.g. `import './language.js'`). Jest's
    // resolver doesn't map .js -> .ts, so strip the extension here.
    '^(\\.{1,2}/.*)\\.js$': '$1',
  },
};
