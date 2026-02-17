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

/**
 * Babel plugin that automatically adds descriptive messages to assertion
 * functions. Transforms calls like `assertExists(foo.bar)` into
 * `assertExists(foo.bar, 'foo.bar')` so that error messages show what
 * expression failed.
 */

const ASSERTION_FUNCTIONS = new Set([
  'assertExists',
  'assertTrue',
  'assertFalse',
  'assertDefined',
  'assertUnreachable',
  'assertIsInstanceOf',
]);

module.exports = function assertMessagesPlugin({types: t}) {
  function getFunctionName(callee) {
    // Simple identifier: assertExists(...)
    if (t.isIdentifier(callee)) {
      return callee.name;
    }
    // Member expression: logging.assertExists(...) or foo.bar.assertExists(...)
    if (t.isMemberExpression(callee) && t.isIdentifier(callee.property)) {
      return callee.property.name;
    }
    // Indirect call pattern: (0, logging.assertExists)(...)
    // This is common after tsc/commonjs transforms
    if (t.isSequenceExpression(callee)) {
      const expressions = callee.expressions;
      if (expressions.length === 2) {
        const lastExpr = expressions[1];
        if (t.isMemberExpression(lastExpr) && t.isIdentifier(lastExpr.property)) {
          return lastExpr.property.name;
        }
      }
    }
    return null;
  }

  return {
    name: 'assert-messages',
    visitor: {
      CallExpression(path) {
        const callee = path.node.callee;
        const funcName = getFunctionName(callee);

        // Check if it's one of our assertion functions
        if (!funcName || !ASSERTION_FUNCTIONS.has(funcName)) {
          return;
        }

        const args = path.node.arguments;

        // If no arguments, skip
        if (args.length === 0) {
          return;
        }

        // If last argument is already a string literal, assume it's a message
        const lastArg = args[args.length - 1];
        if (t.isStringLiteral(lastArg)) {
          return;
        }

        // Extract source text directly from the original code
        const firstArg = args[0];
        const code = path.hub.file.code;
        if (!code || firstArg.start == null || firstArg.end == null) {
          return; // Can't get source text, leave the call unchanged
        }

        const sourceText = code.slice(firstArg.start, firstArg.end);
        args.push(t.stringLiteral(sourceText));
      },
    },
  };
};
