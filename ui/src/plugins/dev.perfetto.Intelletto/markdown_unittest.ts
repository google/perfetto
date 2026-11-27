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

import markdownit from 'markdown-it';
import m from 'mithril';
import {ensureDefined} from '../../base/assert';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {CodeSnippet} from '../../widgets/code_snippet';
import {renderMarkdown} from './markdown';

function renderToDom(children: m.Children): HTMLElement {
  const container = document.createElement('div');
  m.render(container, children);
  return container;
}

describe('renderMarkdown code blocks', () => {
  const md = markdownit({html: false}).disable('image');

  test('renders top-level fenced code block as CodeSnippet with language', () => {
    const vnodes = renderMarkdown(
      md,
      [
        'Here is a query:',
        '',
        '```sql extra-info',
        'SELECT * FROM slice WHERE dur > 1000000;',
        '```',
        '',
        'Done.',
      ].join('\n'),
    );

    const arr = Array.isArray(vnodes) ? vnodes : [vnodes];
    const snippet = arr.find(
      (v): v is m.Vnode<{text: string; language?: string; class?: string}> =>
        typeof v === 'object' &&
        v !== null &&
        'tag' in v &&
        v.tag === CodeSnippet,
    );
    const definedSnippet = ensureDefined(snippet);
    expect(definedSnippet.attrs.text).toBe(
      'SELECT * FROM slice WHERE dur > 1000000;',
    );
    expect(definedSnippet.attrs.language).toBe('sql');
    expect(definedSnippet.attrs.class).toBe('pf-intelletto__code');

    const dom = renderToDom(vnodes);
    expect(dom.querySelector('.pf-code-snippet pre')?.textContent).toBe(
      'SELECT * FROM slice WHERE dur > 1000000;',
    );
    expect(dom.querySelector('.pf-code-snippet-language')?.textContent).toBe(
      'sql',
    );
  });

  test('renders fenced code block without language and indented code blocks', () => {
    const vnodes = renderMarkdown(
      md,
      [
        '```',
        'plain code',
        '```',
        '',
        '    indented line 1',
        '    indented line 2',
      ].join('\n'),
    );
    const dom = renderToDom(vnodes);
    const snippets = dom.querySelectorAll('.pf-code-snippet pre');
    expect(snippets).toHaveLength(2);
    expect(snippets[0]?.textContent).toBe('plain code');
    expect(snippets[1]?.textContent).toBe('indented line 1\nindented line 2');
  });

  test('leaves nested code blocks inside lists as HTML pre elements', () => {
    const vnodes = renderMarkdown(
      md,
      ['- item:', '', '  ```ts', '  const x = 1;', '  ```'].join('\n'),
    );
    const dom = renderToDom(vnodes);
    expect(dom.querySelector('.pf-code-snippet')).toBeNull();
    expect(dom.querySelector('ul li pre code')?.textContent).toContain(
      'const x = 1;',
    );
  });
});

describe('renderMarkdown tables', () => {
  const md = markdownit({html: false}).disable('image');

  test('renders regular markdown tables as HTML preserving inline markup', () => {
    const vnodes = renderMarkdown(
      md,
      [
        '| Property | Value |',
        '| --- | --- |',
        '| **Process** | `com.example.app` |',
      ].join('\n'),
    );
    const dom = renderToDom(vnodes);
    expect(dom.querySelector('.pf-intelletto__table')).toBeNull();
    expect(dom.querySelector('table th')?.textContent).toBe('Property');
    expect(dom.querySelector('table td strong')?.textContent).toBe('Process');
    expect(dom.querySelector('table td code')?.textContent).toBe(
      'com.example.app',
    );
  });

  test('renders ```datagrid fenced table as DataGrid', () => {
    const vnodes = renderMarkdown(
      md,
      [
        '```datagrid',
        '| Name | Count |',
        '| --- | --- |',
        '| foo | 10 |',
        '| bar | 25 |',
        '```',
      ].join('\n'),
    );
    const arr = Array.isArray(vnodes) ? vnodes : [vnodes];
    const wrapper = arr.find(
      (v): v is m.Vnode =>
        typeof v === 'object' &&
        v !== null &&
        'tag' in v &&
        Array.isArray(v.children),
    );
    const definedWrapper = ensureDefined(wrapper);
    const grid = (definedWrapper.children as m.Vnode<unknown>[]).find(
      (c) => (c.tag as unknown) === DataGrid,
    );
    const definedGrid = ensureDefined(grid);
    const attrs = definedGrid.attrs as {
      schema: Record<string, {title: string; columnType: string}>;
      data: Array<Record<string, unknown>>;
    };
    expect(attrs.schema['c0']?.columnType).toBe('text');
    expect(attrs.schema['c1']?.columnType).toBe('quantitative');
    expect(attrs.data).toEqual([
      {c0: 'foo', c1: 10},
      {c0: 'bar', c1: 25},
    ]);
  });

  test('falls back to CodeSnippet when ```datagrid block has no table yet', () => {
    const vnodes = renderMarkdown(
      md,
      ['```datagrid', '| Name |', '```'].join('\n'),
    );
    const dom = renderToDom(vnodes);
    expect(dom.querySelector('.pf-intelletto__table')).toBeNull();
    expect(dom.querySelector('.pf-code-snippet pre')?.textContent).toBe(
      '| Name |',
    );
  });
});
