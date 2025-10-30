// Copyright (C) 2025 The Android Open Source Project
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

import m from 'mithril';
import {LazyTreeNode, Tree, TreeNode} from '../../../widgets/tree';
import {PopupMenu} from '../../../widgets/menu';
import {MenuItem} from '../../../widgets/menu';
import {Anchor} from '../../../widgets/anchor';
import {PopupPosition} from '../../../widgets/popup';
import {Icons} from '../../../base/semantic_icons';
import {renderWidgetShowcase} from '../widgets_page_utils';

function recursiveTreeNode(): m.Children {
  return m(LazyTreeNode, {
    left: 'Recursive',
    right: '...',
    fetchData: async () => {
      // await new Promise((r) => setTimeout(r, 1000));
      return () => recursiveTreeNode();
    },
  });
}

export function renderTree(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Tree'),
      m(
        'p',
        'A hierarchical tree view component with expandable/collapsible nodes for displaying nested data structures.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) =>
        m(
          Tree,
          opts,
          m(TreeNode, {left: 'Name', right: 'my_event', icon: 'badge'}),
          m(TreeNode, {left: 'CPU', right: '2', icon: 'memory'}),
          m(TreeNode, {
            left: 'Start time',
            right: '1s 435ms',
            icon: 'schedule',
          }),
          m(TreeNode, {left: 'Duration', right: '86ms', icon: 'timer'}),
          m(TreeNode, {
            left: 'SQL',
            right: m(
              PopupMenu,
              {
                position: PopupPosition.RightStart,
                trigger: m(
                  Anchor,
                  {
                    icon: Icons.ContextMenu,
                  },
                  'SELECT * FROM ftrace_event WHERE id = 123',
                ),
              },
              m(MenuItem, {
                label: 'Copy SQL Query',
                icon: 'content_copy',
              }),
              m(MenuItem, {
                label: 'Execute Query in new tab',
                icon: 'open_in_new',
              }),
            ),
          }),
          m(TreeNode, {
            icon: 'account_tree',
            left: 'Process',
            right: m(Anchor, {icon: 'open_in_new'}, '/bin/foo[789]'),
          }),
          m(TreeNode, {
            left: 'Thread',
            right: m(Anchor, {icon: 'open_in_new'}, 'my_thread[456]'),
          }),
          m(
            TreeNode,
            {
              left: 'Args',
              summary: 'foo: string, baz: string, quux: string[4]',
            },
            m(TreeNode, {left: 'foo', right: 'bar'}),
            m(TreeNode, {left: 'baz', right: 'qux'}),
            m(
              TreeNode,
              {left: 'quux', summary: 'string[4]'},
              m(TreeNode, {left: '[0]', right: 'corge'}),
              m(TreeNode, {left: '[1]', right: 'grault'}),
              m(TreeNode, {left: '[2]', right: 'garply'}),
              m(TreeNode, {left: '[3]', right: 'waldo'}),
            ),
          ),
          m(LazyTreeNode, {
            left: 'Lazy',
            icon: 'bedtime',
            fetchData: async () => {
              await new Promise((r) => setTimeout(r, 1000));
              return () => m(TreeNode, {left: 'foo'});
            },
          }),
          m(LazyTreeNode, {
            left: 'Dynamic',
            unloadOnCollapse: true,
            icon: 'bedtime',
            fetchData: async () => {
              await new Promise((r) => setTimeout(r, 1000));
              return () => m(TreeNode, {left: 'foo'});
            },
          }),
          recursiveTreeNode(),
        ),
    }),
  ];
}
