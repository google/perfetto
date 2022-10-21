// Copyright (C) 2020 The Android Open Source Project
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


import * as m from 'mithril';

import {Actions} from '../common/actions';

import {globals} from './globals';
import {createPage} from './pages';
import {QueryHistoryComponent, queryHistoryStorage} from './query_history';
import {QueryTable} from './query_table';

const INPUT_PLACEHOLDER = 'Enter query and press Cmd/Ctrl + Enter';
const INPUT_MIN_LINES = 2;
const INPUT_MAX_LINES = 10;
const INPUT_LINE_HEIGHT_EM = 1.2;
const TAB_SPACES = 2;
const TAB_SPACES_STRING = ' '.repeat(TAB_SPACES);
const QUERY_ID = 'analyze-page-query';

class QueryInput implements m.ClassComponent {
  // How many lines to display if the user hasn't resized the input box.
  displayLines = INPUT_MIN_LINES;

  static onKeyDown(e: Event) {
    const event = e as KeyboardEvent;
    const target = e.target as HTMLTextAreaElement;
    const {selectionStart, selectionEnd} = target;

    if (event.code === 'Enter' && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      let query = target.value;
      if (selectionEnd > selectionStart) {
        query = query.substring(selectionStart, selectionEnd);
      }
      if (!query) return;
      queryHistoryStorage.saveQuery(query);
      globals.dispatch(Actions.executeQuery({queryId: QUERY_ID, query}));
    }

    if (event.code === 'Tab') {
      // Handle tabs to insert spaces.
      event.preventDefault();
      const lastLineBreak = target.value.lastIndexOf('\n', selectionEnd);

      if (selectionStart === selectionEnd || lastLineBreak < selectionStart) {
        // Selection does not contain line breaks, therefore is on a single
        // line. In this case, replace the selection with spaces. Replacement is
        // done via document.execCommand as opposed to direct manipulation of
        // element's value attribute because modifying latter programmatically
        // drops the edit history which breaks undo/redo functionality.
        document.execCommand('insertText', false, TAB_SPACES_STRING);
      } else {
        this.handleMultilineTab(target, event);
      }
    }
  }

  // Handle Tab press when the current selection is multiline: find all the
  // lines intersecting with the selection, and either indent or dedent (if
  // Shift key is held) them.
  private static handleMultilineTab(
      target: HTMLTextAreaElement, event: KeyboardEvent) {
    const {selectionStart, selectionEnd} = target;
    const firstLineBreak = target.value.lastIndexOf('\n', selectionStart - 1);

    // If no line break is found (selection begins at the first line),
    // replacementStart would have the correct value of 0.
    const replacementStart = firstLineBreak + 1;
    const replacement = target.value.substring(replacementStart, selectionEnd)
                            .split('\n')
                            .map((line) => {
                              if (event.shiftKey) {
                                // When Shift is held, remove whitespace at the
                                // beginning
                                return this.dedent(line);
                              } else {
                                return TAB_SPACES_STRING + line;
                              }
                            })
                            .join('\n');
    // Select the range to be replaced.
    target.setSelectionRange(replacementStart, selectionEnd);
    document.execCommand('insertText', false, replacement);
    // Restore the selection to match the previous selection, allowing to chain
    // indent operations by just pressing Tab several times.
    target.setSelectionRange(
        replacementStart, replacementStart + replacement.length);
  }

  // Chop off up to TAB_SPACES leading spaces from a string.
  private static dedent(line: string): string {
    let i = 0;
    while (i < line.length && i < TAB_SPACES && line[i] === ' ') {
      i++;
    }
    return line.substring(i);
  }

  onInput(textareaValue: string) {
    const textareaLines = textareaValue.split('\n').length;
    const clampedNumLines =
        Math.min(Math.max(textareaLines, INPUT_MIN_LINES), INPUT_MAX_LINES);
    this.displayLines = clampedNumLines;
    globals.dispatch(Actions.setAnalyzePageQuery({query: textareaValue}));
    globals.rafScheduler.scheduleFullRedraw();
  }

  // This method exists because unfortunatley setting custom properties on an
  // element's inline style attribue doesn't seem to work in mithril, even
  // though the docs claim so.
  setHeightBeforeResize(node: HTMLElement) {
    // +2em for some extra breathing space to account for padding.
    const heightEm = this.displayLines * INPUT_LINE_HEIGHT_EM + 2;
    // We set a height based on the number of lines that we want to display by
    // default. If the user resizes the textbox using the resize handle in the
    // bottom-right corner, this height is overridden.
    node.style.setProperty('--height-before-resize', `${heightEm}em`);
    // TODO(dproy): The resized height is lost if user navigates away from the
    // page and comes back.
  }

  oncreate(vnode: m.VnodeDOM) {
    // This makes sure query persists if user navigates to other pages and comes
    // back to analyze page.
    const existingQuery = globals.state.analyzePageQuery;
    const textarea = vnode.dom as HTMLTextAreaElement;
    if (existingQuery) {
      textarea.value = existingQuery;
      this.onInput(existingQuery);
    }

    this.setHeightBeforeResize(textarea);
  }

  onupdate(vnode: m.VnodeDOM) {
    this.setHeightBeforeResize(vnode.dom as HTMLElement);
  }

  view() {
    return m('textarea.query-input', {
      placeholder: INPUT_PLACEHOLDER,
      onkeydown: (e: Event) => QueryInput.onKeyDown(e),
      oninput: (e: Event) =>
          this.onInput((e.target as HTMLTextAreaElement).value),
    });
  }
}


export const AnalyzePage = createPage({
  view() {
    return m(
        '.analyze-page',
        m(QueryInput),
        m(QueryTable, {queryId: QUERY_ID}),
        m(QueryHistoryComponent));
  },
});
