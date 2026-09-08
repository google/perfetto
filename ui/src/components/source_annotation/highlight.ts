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

// A small dependency-free tokenizer for the source and assembly listings.
// It colours just enough to make code scannable: keywords, types, strings,
// comments and numbers in C, C++ and Rust, and mnemonics, registers and
// numbers in disassembly. Tokens become spans classed
// `pf-source-annotation__syntax--<kind>`.

import m from 'mithril';

export type SourceLanguage = 'c' | 'rust' | 'plain';

type TokenKind =
  | 'keyword'
  | 'type'
  | 'string'
  | 'comment'
  | 'number'
  | 'mnemonic'
  | 'register';

const KEYWORDS = new Set([
  // C / C++.
  'if',
  'else',
  'for',
  'while',
  'do',
  'switch',
  'case',
  'break',
  'continue',
  'return',
  'goto',
  'default',
  'sizeof',
  'typedef',
  'struct',
  'union',
  'enum',
  'class',
  'namespace',
  'template',
  'typename',
  'public',
  'private',
  'protected',
  'virtual',
  'override',
  'final',
  'new',
  'delete',
  'this',
  'nullptr',
  'true',
  'false',
  'using',
  'static',
  'const',
  'constexpr',
  'consteval',
  'volatile',
  'inline',
  'extern',
  'register',
  'auto',
  'operator',
  'throw',
  'try',
  'catch',
  'noexcept',
  'friend',
  'explicit',
  'mutable',
  'static_cast',
  'reinterpret_cast',
  'const_cast',
  'dynamic_cast',
  'decltype',
  'concept',
  'requires',
  'co_await',
  'co_return',
  'co_yield',
  // Rust.
  'fn',
  'let',
  'mut',
  'impl',
  'trait',
  'pub',
  'use',
  'mod',
  'match',
  'loop',
  'in',
  'as',
  'dyn',
  'move',
  'ref',
  'unsafe',
  'async',
  'await',
  'crate',
  'super',
  'where',
  'self',
  'Self',
]);

const TYPES = new Set([
  'int',
  'char',
  'short',
  'long',
  'float',
  'double',
  'unsigned',
  'signed',
  'bool',
  'void',
  'wchar_t',
  'char16_t',
  'char32_t',
  'size_t',
  'ssize_t',
  'ptrdiff_t',
  'intptr_t',
  'uintptr_t',
  'int8_t',
  'int16_t',
  'int32_t',
  'int64_t',
  'uint8_t',
  'uint16_t',
  'uint32_t',
  'uint64_t',
  'u8',
  'u16',
  'u32',
  'u64',
  'u128',
  'usize',
  'i8',
  'i16',
  'i32',
  'i64',
  'i128',
  'isize',
  'f32',
  'f64',
  'str',
]);

const ID_START = /[A-Za-z_$]/;
const ID_PART = /[A-Za-z0-9_$]/;
const DIGIT = /[0-9]/;

// x86 (Intel syntax) and arm64 register names.
const REGISTER =
  /^(?:[er]?[abcd]x|[er]?[sd]i|[er]?[sb]p|r(?:8|9|1[0-5])[dwb]?|[abcd][lh]|[sd]il|[sb]pl|[xyz]mm\d+|k[0-7]|[wx](?:[12]?\d|3[01]|zr)|sp|lr|pc|fp|[vqdshb]\d+|[cdefgs]s|[er]?ip|[er]?flags)$/i;

export function languageForPath(path: string | undefined): SourceLanguage {
  if (path === undefined) return 'plain';
  const dot = path.lastIndexOf('.');
  const ext = dot === -1 ? '' : path.slice(dot + 1).toLowerCase();
  if (ext === 'rs') return 'rust';
  if (
    ['c', 'cc', 'cpp', 'cxx', 'h', 'hh', 'hpp', 'hxx', 'm', 'mm'].includes(ext)
  ) {
    return 'c';
  }
  return 'plain';
}

function span(kind: TokenKind, text: string): m.Children {
  return m(`span.pf-source-annotation__syntax--${kind}`, text);
}

export interface HighlightedLine {
  readonly children: m.Children;
  // Whether a block comment is still open at the end of the line, to be
  // passed to the next line.
  readonly inBlockComment: boolean;
}

// Tokenizes one line of source. Block comments spanning lines are tracked
// through `inBlockComment`.
export function highlightSourceLine(
  line: string,
  language: SourceLanguage,
  inBlockComment: boolean,
): HighlightedLine {
  if (language === 'plain') {
    return {children: line, inBlockComment: false};
  }
  const out: m.Children[] = [];
  const n = line.length;
  let i = 0;
  let open = inBlockComment;
  while (i < n) {
    if (open) {
      const end = line.indexOf('*/', i);
      if (end === -1) {
        out.push(span('comment', line.slice(i)));
        i = n;
      } else {
        out.push(span('comment', line.slice(i, end + 2)));
        i = end + 2;
        open = false;
      }
      continue;
    }
    const c = line[i];
    const pair = line.slice(i, i + 2);
    if (pair === '//') {
      out.push(span('comment', line.slice(i)));
      break;
    }
    if (pair === '/*') {
      const end = line.indexOf('*/', i + 2);
      if (end === -1) {
        out.push(span('comment', line.slice(i)));
        open = true;
        i = n;
      } else {
        out.push(span('comment', line.slice(i, end + 2)));
        i = end + 2;
      }
      continue;
    }
    if (c === '"' || c === "'") {
      const start = i;
      i++;
      while (i < n) {
        if (line[i] === '\\') {
          i += 2;
          continue;
        }
        if (line[i] === c) {
          i++;
          break;
        }
        i++;
      }
      out.push(span('string', line.slice(start, i)));
      continue;
    }
    if (DIGIT.test(c)) {
      const start = i;
      while (i < n && /[0-9a-fA-FxXbBoO._']/.test(line[i])) i++;
      // Suffixes: 10u, 10ul, 1.0f and Rust's 10usize.
      while (i < n && /[uUlLfFiIsSzZe]/.test(line[i])) i++;
      out.push(span('number', line.slice(start, i)));
      continue;
    }
    if (ID_START.test(c)) {
      const start = i;
      while (i < n && ID_PART.test(line[i])) i++;
      const word = line.slice(start, i);
      if (TYPES.has(word)) {
        out.push(span('type', word));
      } else if (KEYWORDS.has(word)) {
        out.push(span('keyword', word));
      } else {
        out.push(word);
      }
      continue;
    }
    // A run of operators, punctuation and whitespace.
    const start = i;
    i++;
    while (
      i < n &&
      !ID_START.test(line[i]) &&
      !DIGIT.test(line[i]) &&
      line[i] !== '"' &&
      line[i] !== "'" &&
      line.slice(i, i + 2) !== '//' &&
      line.slice(i, i + 2) !== '/*'
    ) {
      i++;
    }
    out.push(line.slice(start, i));
  }
  return {children: out, inBlockComment: open};
}

// Tokenizes one disassembled instruction: the mnemonic, then registers and
// numbers among the operands.
export function highlightInstruction(text: string): m.Children {
  const parts = text.split(/(\s+|,|\(|\)|\[|\]|\+|\*|:|<|>|!|#)/);
  const out: m.Children[] = [];
  let seenMnemonic = false;
  for (const part of parts) {
    if (part === '') continue;
    if (!seenMnemonic && /^[a-z][a-z0-9.]*$/i.test(part)) {
      seenMnemonic = true;
      out.push(span('mnemonic', part));
    } else if (/^-?0x[0-9a-f]+$/i.test(part) || /^-?\d+$/.test(part)) {
      out.push(span('number', part));
    } else if (REGISTER.test(part)) {
      out.push(span('register', part));
    } else {
      out.push(part);
    }
  }
  return out;
}
