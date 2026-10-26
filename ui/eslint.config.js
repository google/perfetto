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

const fs = require('fs');
const globals = require('globals');
const jsdoc = require('eslint-plugin-jsdoc');
const path = require('node:path');
const tsParser = require('@typescript-eslint/parser');
const typescriptEslint = require('@typescript-eslint/eslint-plugin');

const ignorePath = path.resolve(__dirname, '.prettierignore');
const ignores = fs
    .readFileSync(ignorePath, {encoding: 'utf8'})
    .split('\n')
    .filter((l) => l !== '' && !l.startsWith('#'));

module.exports = [
  // `ignores` has to go on a standalone block at the start otherwise gets
  // overridden by configs, because the new eslint flat config is so clever.
  {ignores: ignores},

  jsdoc.configs['flat/recommended'],

  {
    files: ['src/**/*.ts'],
    plugins: {
      '@typescript-eslint': typescriptEslint,
      jsdoc,
    },

    languageOptions: {
      globals: {
        ...globals.browser,
        ...globals.node,
      },
      ecmaVersion: 'latest',
      sourceType: 'module',
      parser: tsParser,
      parserOptions: {
        project: './tsconfig.json',
      },
    },

    rules: {
      'curly': ['error', 'multi-line'],
      'guard-for-in': 'error',
      'no-caller': 'error',
      'no-extend-native': 'error',
      'no-extra-bind': 'error',
      'no-invalid-this': 'error',
      'no-multi-str': 'error',
      'no-new-wrappers': 'error',
      'no-throw-literal': 'error',
      'no-with': 'error',
      'prefer-promise-reject-errors': 'error',
      'no-var': 'error',
      'prefer-const': ['error', {destructuring: 'all'}],
      'prefer-spread': 'error',
      'one-var': ['error', {var: 'never', let: 'never', const: 'never'}],
      'spaced-comment': ['error', 'always'],

      'no-multi-spaces': [
        'error',
        {
          ignoreEOLComments: true,
        },
      ],

      'no-unused-vars': 'off',

      '@typescript-eslint/no-unused-vars': [
        'error',
        {
          argsIgnorePattern: '^_.*',
          varsIgnorePattern: '^_.*',
        },
      ],

      'no-array-constructor': 'off',
      '@typescript-eslint/no-array-constructor': ['error'],
      'prefer-rest-params': 'off',

      'new-cap': [
        'error',
        {
          capIsNew: false,
          properties: false,
        },
      ],

      'jsdoc/require-jsdoc': 'off',
      'jsdoc/require-param': 'off',
      'jsdoc/require-param-type': 'off',
      'jsdoc/require-returns': 'off',
      'jsdoc/require-returns-type': 'off',
      'jsdoc/tag-lines': 'off',
      'jsdoc/check-tag-names': [
        'error',
        {
          definedTags: ['experimental'],
        },
      ],

      '@typescript-eslint/no-explicit-any': 'error',

      '@typescript-eslint/strict-boolean-expressions': [
        'error',
        {
          allowNullableBoolean: true,
          allowNullableObject: true,
          allowNullableString: true,
        },
      ],

      '@typescript-eslint/consistent-type-imports': ['error', {
        prefer: 'type-imports',
        fixStyle: 'inline-type-imports',
        disallowTypeAnnotations: true,
      }],
      '@typescript-eslint/no-import-type-side-effects': 'error',
    },
  },
];
