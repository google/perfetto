import m from 'mithril';
import {classNames} from '../classnames';
import {globals} from '../globals';
import {Button} from './button';
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
  left?: m.Child;
  // Content to display in the right hand column.
  // If omitted, this side will be left blank.
  right?: m.Child;
  // Content to display in the right hand column when the node is collapsed.
  // If omitted, the value of `right` shall be shown when collapsed instead.
  // If the node has no children, this value is never shown.
  summary?: m.Child;
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
    if (hasChildren(vnode) && this.collapsed) {
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
