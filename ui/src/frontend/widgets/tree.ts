import m from 'mithril';

import {classNames} from '../classnames';
import {globals} from '../globals';

import {Button} from './button';
import {Spinner} from './spinner';
import {hasChildren} from './utils';

export enum TreeLayout {
  // Classic heirachical tree layout with no columnar alignment.
  // Example:
  // foo: bar
  //  ├ baz: qux
  //  └ quux: corge
  // grault: garply
  Tree = 'tree',

  // Heirachical tree layout but right values are horizontally aligned.
  // Example:
  // foo     bar
  //  ├ baz  qux
  //  └ quux corge
  // grault  garply
  Grid = 'grid',
}

interface TreeAttrs {
  // The style of layout.
  // Defaults to grid.
  layout?: TreeLayout;
  // Space delimited class list applied to our tree element.
  className?: string;
}

export class Tree implements m.ClassComponent<TreeAttrs> {
  view({attrs, children}: m.Vnode<TreeAttrs>): m.Children {
    const {
      layout: style = TreeLayout.Grid,
      className = '',
    } = attrs;

    if (style === TreeLayout.Grid) {
      return m('.pf-ptree-grid', {class: className}, children);
    } else if (style === TreeLayout.Tree) {
      return m('.pf-ptree', {class: className}, children);
    } else {
      return null;
    }
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
  // Called when the collapsed state is changed, mainly used in controlled mode.
  onCollapseChanged?: (collapsed: boolean, attrs: TreeNodeAttrs) => void;
}

export class TreeNode implements m.ClassComponent<TreeNodeAttrs> {
  private collapsed = false;
  view(vnode: m.CVnode<TreeNodeAttrs>): m.Children {
    return [
      m(
          '.pf-tree-node',
          this.renderLeft(vnode),
          this.renderRight(vnode),
          ),
      hasChildren(vnode) && this.renderChildren(vnode),
    ];
  }

  private renderLeft(vnode: m.CVnode<TreeNodeAttrs>) {
    const {
      attrs: {left},
    } = vnode;

    return m(
        '.pf-tree-left',
        left,
        hasChildren(vnode) && this.renderCollapseButton(vnode),
    );
  }

  private renderRight(vnode: m.CVnode<TreeNodeAttrs>) {
    const {attrs: {right, summary}} = vnode;
    if (hasChildren(vnode) && this.isCollapsed(vnode)) {
      return m('.pf-tree-right', summary ?? right);
    } else {
      return m('.pf-tree-right', right);
    }
  }

  private renderChildren(vnode: m.CVnode<TreeNodeAttrs>) {
    const {children} = vnode;

    return m(
        '.pf-tree-children',
        {
          class: classNames(this.isCollapsed(vnode) && 'pf-pgrid-hidden'),
        },
        children,
    );
  }

  private renderCollapseButton(vnode: m.Vnode<TreeNodeAttrs>) {
    const {attrs, attrs: {onCollapseChanged = () => {}}} = vnode;

    return m(Button, {
      icon: this.isCollapsed(vnode) ? 'chevron_right' : 'expand_more',
      minimal: true,
      compact: true,
      onclick: () => {
        this.collapsed = !this.isCollapsed(vnode);
        onCollapseChanged(this.collapsed, attrs);
        globals.rafScheduler.scheduleFullRedraw();
      },
    });
  }

  private isCollapsed({attrs}: m.Vnode<TreeNodeAttrs>): boolean {
    // If collapsed is omitted, use our local collapsed state instead.
    const {
      collapsed = this.collapsed,
    } = attrs;

    return collapsed;
  }
}

export function dictToTree(dict: {[key: string]: m.Child}): m.Children {
  const children: m.Child[] = [];
  for (const key of Object.keys(dict)) {
    children.push(m(TreeNode, {
      left: key,
      right: dict[key],
    }));
  }
  return m(Tree, children);
}

interface LazyTreeNodeAttrs {
  // Same as TreeNode (see above).
  left?: m.Children;
  // Same as TreeNode (see above).
  right?: m.Children;
  // Same as TreeNode (see above).
  summary?: m.Children;
  // A callback to be called when the TreeNode is expanded, in order to fetch
  // child nodes.
  // The callback must return a promise to a function which returns m.Children.
  // The reason the promise must return a function rather than the actual
  // children is to avoid storing vnodes between render cycles, which is a bug
  // in Mithril.
  fetchData: () => Promise<() => m.Children>;
  // Whether to keep child nodes in memory after the node has been collapsed.
  // Defaults to true
  hoardData?: boolean;
}

// This component is a TreeNode which only loads child nodes when it's expanded.
// This allows us to represent huge trees without having to load all the data
// up front, and even allows us to represent infinite or recursive trees.
export class LazyTreeNode implements m.ClassComponent<LazyTreeNodeAttrs> {
  private collapsed: boolean = true;
  private renderChildren = this.renderSpinner;

  private renderSpinner(): m.Children {
    return m(TreeNode, {left: m(Spinner)});
  }

  view({attrs}: m.CVnode<LazyTreeNodeAttrs>): m.Children {
    const {
      left,
      right,
      summary,
      fetchData,
      hoardData = true,
    } = attrs;

    return m(
        TreeNode,
        {
          left,
          right,
          summary,
          collapsed: this.collapsed,
          onCollapseChanged: (collapsed) => {
            if (collapsed) {
              if (!hoardData) {
                this.renderChildren = this.renderSpinner;
              }
            } else {
              fetchData().then((result) => {
                if (!this.collapsed) {
                  this.renderChildren = result;
                  globals.rafScheduler.scheduleFullRedraw();
                }
              });
            }
            this.collapsed = collapsed;
            globals.rafScheduler.scheduleFullRedraw();
          },
        },
        this.renderChildren());
  }
}
