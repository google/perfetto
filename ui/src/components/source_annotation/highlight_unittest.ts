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

import type m from 'mithril';
import {
  highlightInstruction,
  highlightSourceLine,
  languageForPath,
} from './highlight';

// Flattens rendered children into [kind, text] pairs, with plain text
// carrying an undefined kind.
function tokens(children: m.Children): Array<[string | undefined, string]> {
  const out: Array<[string | undefined, string]> = [];
  const visit = (c: m.Children) => {
    if (Array.isArray(c)) {
      c.forEach(visit);
    } else if (typeof c === 'string') {
      out.push([undefined, c]);
    } else if (c !== null && typeof c === 'object' && 'tag' in c) {
      const className = String(c.attrs?.className ?? '');
      const kind = className.replace('pf-source-annotation__syntax--', '');
      out.push([kind, textOf(c)]);
    }
  };
  // The text of a vnode: text vnodes have tag '#'.
  const textOf = (v: m.Vnode): string => {
    if (v.tag === '#') return String(v.children);
    if (typeof v.children === 'string') return v.children;
    if (Array.isArray(v.children)) {
      return v.children
        .map((child) =>
          child !== null && typeof child === 'object' && 'tag' in child
            ? textOf(child)
            : String(child ?? ''),
        )
        .join('');
    }
    return '';
  };
  visit(children);
  return out;
}

describe('languageForPath', () => {
  it('maps extensions', () => {
    expect(languageForPath('/src/a.cc')).toBe('c');
    expect(languageForPath('/src/a.h')).toBe('c');
    expect(languageForPath('/src/lib.rs')).toBe('rust');
    expect(languageForPath('/src/notes.txt')).toBe('plain');
    expect(languageForPath(undefined)).toBe('plain');
  });
});

describe('highlightSourceLine', () => {
  it('classifies keywords, types, numbers and strings', () => {
    const {children} = highlightSourceLine(
      'const int x = 0x10; return "a\\"b";',
      'c',
      false,
    );
    expect(tokens(children)).toEqual([
      ['keyword', 'const'],
      [undefined, ' '],
      ['type', 'int'],
      [undefined, ' '],
      [undefined, 'x'],
      [undefined, ' = '],
      ['number', '0x10'],
      [undefined, '; '],
      ['keyword', 'return'],
      [undefined, ' '],
      ['string', '"a\\"b"'],
      [undefined, ';'],
    ]);
  });

  it('threads block comments across lines', () => {
    const first = highlightSourceLine('int a; /* start', 'c', false);
    expect(first.inBlockComment).toBe(true);
    expect(tokens(first.children)).toEqual([
      ['type', 'int'],
      [undefined, ' '],
      [undefined, 'a'],
      [undefined, '; '],
      ['comment', '/* start'],
    ]);
    const second = highlightSourceLine('end */ x++; // done', 'c', true);
    expect(second.inBlockComment).toBe(false);
    expect(tokens(second.children)).toEqual([
      ['comment', 'end */'],
      [undefined, ' '],
      [undefined, 'x'],
      [undefined, '++; '],
      ['comment', '// done'],
    ]);
  });

  it('leaves plain files untouched', () => {
    const {children} = highlightSourceLine('int a;', 'plain', false);
    expect(children).toBe('int a;');
  });
});

describe('highlightInstruction', () => {
  it('classifies the mnemonic, registers and numbers', () => {
    expect(
      tokens(highlightInstruction('mov rax, qword ptr [rbp - 0x8]')),
    ).toEqual([
      ['mnemonic', 'mov'],
      [undefined, ' '],
      ['register', 'rax'],
      [undefined, ','],
      [undefined, ' '],
      [undefined, 'qword'],
      [undefined, ' '],
      [undefined, 'ptr'],
      [undefined, ' '],
      [undefined, '['],
      ['register', 'rbp'],
      [undefined, ' '],
      [undefined, '-'],
      [undefined, ' '],
      ['number', '0x8'],
      [undefined, ']'],
    ]);
  });

  it('handles arm64 syntax', () => {
    expect(
      tokens(highlightInstruction('b.lt 0x1000003ac <_compute+0x78>')),
    ).toEqual([
      ['mnemonic', 'b.lt'],
      [undefined, ' '],
      ['number', '0x1000003ac'],
      [undefined, ' '],
      [undefined, '<'],
      [undefined, '_compute'],
      [undefined, '+'],
      ['number', '0x78'],
      [undefined, '>'],
    ]);
  });
});
