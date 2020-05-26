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
import {QueryTable} from './query_table';

const INPUT_PLACEHOLDER = 'Enter query and press Cmd/Ctrl + Enter';
const INPUT_MIN_LINES = 2;
const INPUT_MAX_LINES = 10;
const INPUT_LINE_HEIGHT_EM = 1.2;
const TAB_SPACES = 2;
const QUERY_ID = 'analyze-page-query';

class QueryInput implements m.ClassComponent {
  // How many lines to display if the user hasn't resized the input box.
  displayLines = INPUT_MIN_LINES;

  static onKeyDown(e: Event) {
    const event = e as KeyboardEvent;
    const target = e.target as HTMLTextAreaElement;

    if (event.code === 'Enter' && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      const query = target.value;
      if (!query) return;
      globals.dispatch(
          Actions.executeQuery({engineId: '0', queryId: QUERY_ID, query}));
    }

    if (event.code === 'Tab') {
      // Handle tabs to insert spaces.
      event.preventDefault();
      const whitespace = ' '.repeat(TAB_SPACES);
      const {selectionStart, selectionEnd} = target;
      target.value = target.value.substring(0, selectionStart) + whitespace +
          target.value.substring(selectionEnd);
      target.selectionEnd = selectionStart + TAB_SPACES;
    }
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
    );
  }
});
