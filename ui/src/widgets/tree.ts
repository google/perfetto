// Copyright (C) 2023 The Android Open Source Project
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

import {classNames} from '../base/classnames';
import {hasChildren} from '../base/mithril_utils';

import {scheduleFullRedraw} from './raf';

// Heirachical tree layout but right values are horizontally aligned.
// Example:
// foo     bar
//  ├ baz  qux
//  └ quux corge
// grault  garply

interface TreeAttrs {
  // Space delimited class list applied to our tree element.
  className?: string;
}

export class Tree implements m.ClassComponent<TreeAttrs> {
  view({attrs, children}: m.Vnode<TreeAttrs>): m.Children {
    const {
      className = '',
    } = attrs;

    const classes = classNames(
        className,
    );

    return m('.pf-tree', {class: classes}, children);
  }
}

interface TreeNodeAttrs {
  // Content to display in the left hand column.
  // If omitted, this side will be blank.
  left?: m.Children;
  // Content to display in the right hand column.
  // If omitted, this side will be left blank.
  right?: m.Children;
  // Content to display in the right hand column when the node is collapsed.
  // If omitted, the value of `right` shall be shown when collapsed instead.
  // If the node has no children, this value is never shown.
  summary?: m.Children;
  // Whether this node is collapsed or not.
  // If omitted, collapsed state 'uncontrolled' - i.e. controlled internally.
  collapsed?: boolean;
  // Whether the node should start collapsed or not, default: false.
  startsCollapsed?: boolean;
  loading?: boolean;
  showCaret?: boolean;
  // Optional icon to show to the left of the text.
  // If this node contains children, this icon is ignored.
  icon?: string;
  // Called when the collapsed state is changed, mainly used in controlled mode.
  onCollapseChanged?: (collapsed: boolean, attrs: TreeNodeAttrs) => void;
}

export class TreeNode implements m.ClassComponent<TreeNodeAttrs> {
  private collapsed;

  constructor({attrs}: m.CVnode<TreeNodeAttrs>) {
    this.collapsed = attrs.startsCollapsed ?? false;
  }

  view(vnode: m.CVnode<TreeNodeAttrs>): m.Children {
    const {children, attrs, attrs: {left, onCollapseChanged = () => {}}} =
        vnode;
    return m(
        '.pf-tree-node',
        {
          class: classNames(this.getClassNameForNode(vnode)),
        },
        m('span.pf-tree-gutter', {
          onclick: () => {
            this.collapsed = !this.isCollapsed(vnode);
            onCollapseChanged(this.collapsed, attrs);
            scheduleFullRedraw();
          },
        }),
        m(
            '.pf-tree-content',
            m('.pf-tree-left', left),
            this.renderRight(vnode),
            ),
        hasChildren(vnode) &&
            [
              m('span.pf-tree-indent-gutter'),
              m('.pf-tree-children', children),
            ],
    );
  }

  private getClassNameForNode(vnode: m.CVnode<TreeNodeAttrs>) {
    const {
      loading = false,
      showCaret = false,
    } = vnode.attrs;
    if (loading) {
      return 'pf-loading';
    } else if (hasChildren(vnode) || showCaret) {
      if (this.isCollapsed(vnode)) {
        return 'pf-collapsed';
      } else {
        return 'pf-expanded';
      }
    } else {
      return undefined;
    }
  }

  private renderRight(vnode: m.CVnode<TreeNodeAttrs>) {
    const {attrs: {right, summary}} = vnode;
    if (hasChildren(vnode) && this.isCollapsed(vnode)) {
      return m('.pf-tree-right', summary ?? right);
    } else {
      return m('.pf-tree-right', right);
    }
  }

  private isCollapsed({attrs}: m.Vnode<TreeNodeAttrs>): boolean {
    // If collapsed is omitted, use our local collapsed state instead.
    const {
      collapsed = this.collapsed,
    } = attrs;

    return collapsed;
  }
}

export function dictToTreeNodes(dict: {[key: string]: m.Child}): m.Child[] {
  const children: m.Child[] = [];
  for (const key of Object.keys(dict)) {
    if (dict[key] == undefined) {
      continue;
    }
    children.push(m(TreeNode, {
      left: key,
      right: dict[key],
    }));
  }
  return children;
}

// Create a flat tree from a POJO
export function dictToTree(dict: {[key: string]: m.Child}): m.Children {
  return m(Tree, dictToTreeNodes(dict));
}
interface LazyTreeNodeAttrs {
  // Same as TreeNode (see above).
  left?: m.Children;
  // Same as TreeNode (see above).
  right?: m.Children;
  // Same as TreeNode (see above).
  icon?: string;
  // Same as TreeNode (see above).
  summary?: m.Children;
  // A callback to be called when the TreeNode is expanded, in order to fetch
  // child nodes.
  // The callback must return a promise to a function which returns m.Children.
  // The reason the promise must return a function rather than the actual
  // children is to avoid storing vnodes between render cycles, which is a bug
  // in Mithril.
  fetchData: () => Promise<() => m.Children>;
  // Whether to unload children on collapse.
  // Defaults to false, data will be kept in memory until the node is destroyed.
  unloadOnCollapse?: boolean;
}

// This component is a TreeNode which only loads child nodes when it's expanded.
// This allows us to represent huge trees without having to load all the data
// up front, and even allows us to represent infinite or recursive trees.
export class LazyTreeNode implements m.ClassComponent<LazyTreeNodeAttrs> {
  private collapsed: boolean = true;
  private loading: boolean = false;
  private renderChildren?: () => m.Children;

  view({attrs}: m.CVnode<LazyTreeNodeAttrs>): m.Children {
    const {
      left,
      right,
      icon,
      summary,
      fetchData,
      unloadOnCollapse = false,
    } = attrs;

    return m(
        TreeNode,
        {
          left,
          right,
          icon,
          summary,
          showCaret: true,
          loading: this.loading,
          collapsed: this.collapsed,
          onCollapseChanged: (collapsed) => {
            if (collapsed) {
              if (unloadOnCollapse) {
                this.renderChildren = undefined;
              }
            } else {
              // Expanding
              if (this.renderChildren) {
                this.collapsed = false;
                scheduleFullRedraw();
              } else {
                this.loading = true;
                fetchData().then((result) => {
                  this.loading = false;
                  this.collapsed = false;
                  this.renderChildren = result;
                  scheduleFullRedraw();
                });
              }
            }
            this.collapsed = collapsed;
            scheduleFullRedraw();
          },
        },
        this.renderChildren && this.renderChildren());
  }
}
