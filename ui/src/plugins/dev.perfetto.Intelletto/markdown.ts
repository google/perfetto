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

import type MarkdownIt from 'markdown-it';
import m from 'mithril';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {ColumnSchema} from '../../components/widgets/datagrid/datagrid_schema';
import type {Row} from '../../trace_processor/query_result';
import {CodeSnippet} from '../../widgets/code_snippet';

type Token = ReturnType<MarkdownIt['parse']>[number];

// A plain number, e.g. "42", "-1.5", "1e6". Cells matching this become numbers
// so they sort numerically in the grid.
const NUMBER_RE = /^-?\d+(\.\d+)?([eE][-+]?\d+)?$/;

/**
 * Render markdown, replacing top-level ```datagrid fenced blocks (containing a
 * markdown table) with interactive DataGrids (sortable, filterable) and other
 * top-level code blocks with CodeSnippet widgets (horizontally scrollable with
 * a copy button). Regular markdown tables and everything else are rendered by
 * markdown-it as HTML so informational/key-value tables keep inline formatting;
 * code blocks nested inside other blocks (lists, quotes) are also left as HTML.
 */
export function renderMarkdown(md: MarkdownIt, text: string): m.Children {
  const tokens = md.parse(text, {});
  const out: m.Children[] = [];
  let start = 0;
  const flushHtml = (end: number) => {
    if (end > start) {
      out.push(
        m.trust(md.renderer.render(tokens.slice(start, end), md.options, {})),
      );
    }
  };
  for (let i = 0; i < tokens.length; i++) {
    const tok = tokens[i];
    if (tok === undefined || tok.level !== 0) continue;
    if (tok.type === 'fence' || tok.type === 'code_block') {
      flushHtml(i);
      const lang = tok.info.trim().split(/\s+/)[0]?.toLowerCase();
      if (tok.type === 'fence' && lang === 'datagrid') {
        const grid = tryRenderDataGrid(md, tok.content);
        out.push(grid ?? renderCodeBlock(tok));
      } else {
        out.push(renderCodeBlock(tok));
      }
      start = i + 1;
    }
  }
  flushHtml(tokens.length);
  return out;
}

function tryRenderDataGrid(
  md: MarkdownIt,
  content: string,
): m.Children | undefined {
  const inner = md.parse(content, {});
  const openIdx = inner.findIndex(
    (t) => t.type === 'table_open' && t.level === 0,
  );
  if (openIdx === -1) return undefined;
  const closeIdx = inner.findIndex(
    (t, j) => j > openIdx && t.type === 'table_close' && t.level === 0,
  );
  if (closeIdx === -1) return undefined;
  return renderTable(inner.slice(openIdx, closeIdx + 1));
}

function renderCodeBlock(tok: Token): m.Children {
  const language = tok.info.trim().split(/\s+/)[0] || undefined;
  return m(CodeSnippet, {
    class: 'pf-intelletto__code',
    text: tok.content.replace(/\n$/, ''),
    language,
  });
}

// Plain text of an inline token: text and inline code, markup dropped.
function inlineText(tok: Token): string {
  return (tok.children ?? [])
    .filter((c) => c.type === 'text' || c.type === 'code_inline')
    .map((c) => c.content)
    .join('');
}

function renderTable(tokens: readonly Token[]): m.Children {
  const headers: string[] = [];
  const cells: string[][] = [];
  let inHead = false;
  for (const tok of tokens) {
    switch (tok.type) {
      case 'thead_open':
        inHead = true;
        break;
      case 'thead_close':
        inHead = false;
        break;
      case 'tr_open':
        if (!inHead) cells.push([]);
        break;
      case 'inline':
        if (inHead) {
          headers.push(inlineText(tok));
        } else {
          cells[cells.length - 1]?.push(inlineText(tok));
        }
        break;
    }
  }

  // Column keys are positional: header text can be empty or duplicated.
  const keys = headers.map((_, i) => `c${i}`);
  const numeric = keys.map((_, i) =>
    cells.every((r) => r[i] === undefined || NUMBER_RE.test(r[i])),
  );
  const schema: ColumnSchema = {};
  keys.forEach((key, i) => {
    schema[key] = {
      title: headers[i] || key,
      titleString: headers[i] || key,
      columnType: numeric[i] ? 'quantitative' : 'text',
    };
  });
  const rows: Row[] = cells.map((r) => {
    const row: Row = {};
    keys.forEach((key, i) => {
      const v = r[i] ?? null;
      row[key] = v !== null && numeric[i] ? Number(v) : v;
    });
    return row;
  });

  return m(
    '.pf-intelletto__table',
    m(DataGrid, {
      schema,
      data: rows,
      initialColumns: keys.map((k) => ({id: k, field: k})),
    }),
  );
}
