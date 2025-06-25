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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Modal, Tree, TreeDataNode, TreeProps, ConfigProvider} from 'antd';
import {Component, createRef, RefObject} from 'react';
import {ElementTreeViewProps, ElementTreeViewState, LynxElement} from './types';
import {constructElementDetail} from './utils';

interface ElementTreeDataNode extends TreeDataNode {
  key: string;
  children: ElementTreeDataNode[];
  parentNode?: ElementTreeDataNode;
  expanded?: boolean;
}

/**
 * Element Tree Visualization Component
 * 
 * Displays a hierarchical tree view of Lynx elements with selection capabilities.
 * Uses Ant Design's Tree component with custom styling and behavior.
 */
export class ElementTreeView extends Component<
  ElementTreeViewProps,
  ElementTreeViewState
> {
  /**
   * Tree data structure for Ant Design Tree component
   */
  private treeData: ElementTreeDataNode[] ;
  private keysToElementTreeDataNodeMap: Map<string, ElementTreeDataNode>;
  /**
   * Reference to the container div for scroll management
   */
  private containerRef: RefObject<HTMLDivElement | null>;
  constructor(props: ElementTreeViewProps) {
    super(props);
    this.treeData = [];
    this.keysToElementTreeDataNodeMap = new Map<string, ElementTreeDataNode>();
    if (props.rootElement && props.selectedElement) {
      this.treeData = this.constructTreeData(
        props.rootElement,
        props.selectedElement,
      );
    }
    this.state = {
      treeHeight: window.innerHeight - 100,
      treeWidth: window.innerWidth - 200,
      expandedKeys: this.filterExpandedKeys(),
      selectedKeys: [props.selectedElement?.id.toString() ?? ''],
      autoExpandParent: true,
    };
    this.containerRef = createRef();
    this.handleKeyDown = this.handleKeyDown.bind(this);
  }

  filterExpandedKeys = () => {
    const expandedKeys: string[] = [];
    const traverse = (node: ElementTreeDataNode) => {
      if (node.expanded) {
        expandedKeys.push(node.key as string);
      }
      if (node.children.length > 0) {
        node.children.forEach((child: ElementTreeDataNode) => {
          traverse(child);
        });
      }
    };
    if (this.treeData.length > 0) {
      traverse(this.treeData[0]);
    }
    return expandedKeys;
  };

  updateTreeSize = () => {
    this.setState({
      treeHeight: window.innerHeight - 100,
      treeWidth: window.innerWidth - 200,
    });
  };

  /**
   * Converts Lynx element hierarchy to TreeDataNode structure
   * @param rootElement - Root element of the tree
   * @returns Array of TreeDataNode for Ant Design Tree
   */
  constructTreeData = (
    rootElement: LynxElement,
    selectedElement: LynxElement,
  ) => {
    const constructTreeDataRecursively = (
      current: LynxElement,
      parentNode: ElementTreeDataNode | undefined,
    ): ElementTreeDataNode | undefined => {
      const currentTreeNode: ElementTreeDataNode = {
        title: constructElementDetail(current),
        key: current.id.toString(),
        children: [],
        parentNode,
        expanded: false,
      };
      this.keysToElementTreeDataNodeMap.set(
        current.id.toString(),
        currentTreeNode,
      );

      for (let i = 0; i < current.children.length; i++) {
        const child = constructTreeDataRecursively(
          current.children[i],
          currentTreeNode,
        );
        if (child) {
          currentTreeNode.children?.push(child);
        }
      }

      // If current element is selected, traversal to root and mark all nodes as show: true
      if (current === selectedElement) {
        if (current.children.length > 0) {
          currentTreeNode.expanded = true;
        }
        let parentNode = currentTreeNode.parentNode;
        while (parentNode !== undefined) {
          parentNode.expanded = true;
          parentNode = parentNode.parentNode;
        }
      }
      return currentTreeNode;
    };

    const treeData: ElementTreeDataNode[] = [];
    const rootTree = constructTreeDataRecursively(rootElement, undefined);
    if (!rootTree) {
      return treeData;
    }
    treeData.push(rootTree);
    return treeData;
  };

  componentDidMount() {
    window.addEventListener('resize', this.updateTreeSize);

    const treeElement = this.containerRef.current?.querySelector(
      `.ant-tree-node-selected`,
    );
    if (treeElement) {
      treeElement.scrollIntoView({
        behavior: 'smooth',
        block: 'center',
      });
    }
  }

  componentWillUnmount() {
    window.removeEventListener('resize', this.updateTreeSize);
  }

  handleKeyDown(event: React.KeyboardEvent) {
    const handlers : Record<string, (node: ElementTreeDataNode | undefined) => void> = {
      'w': this.moveUpSelectNode,
      'arrowup': this.moveUpSelectNode,
      'a': this.collapseCurrentNode,
      'arrowleft': this.collapseCurrentNode,
      's': this.moveDownSelectNode,
      'arrowdown': this.moveDownSelectNode,
      'd': this.expandCurrentNode,
      'arrowright': this.expandCurrentNode,
    };

    const handler = handlers[event.key.toLowerCase()];
    if (handler != null) {
      handler.call(this, this.keysToElementTreeDataNodeMap.get(this.state.selectedKeys[0]));
      event.stopPropagation();
      event.preventDefault();
    }
  }

  render() {
    if (!this.props.rootElement || !this.props.selectedElement) {
      return <div></div>;
    }

    return (
      <div onKeyDown={this.handleKeyDown}>
        <Modal
          open={true}
          centered={true}
          closable={true}
          height={this.state.treeHeight}
          width={this.state.treeWidth}
          onCancel={() => {
            this.props.closeDialog();
          }}
          style={{
            pointerEvents: 'auto',
            borderRadius: 6,
            background: 'white',
          }}
          modalRender={() => (
            <div
              ref={this.containerRef}
              style={{
                overflow: 'auto',
                maxHeight: this.state.treeHeight,
                width: this.state.treeWidth,
                borderRadius: 6,
              }}>
              <ConfigProvider
                theme={{
                  components: {
                    Tree: {
                      nodeSelectedBg: '#6BACDE',
                    },
                  },
                }}>
                <Tree
                  style={{
                    fontSize: 14,
                    fontFamily: 'Roboto Condensed',
                    color: '#121212',
                  }}
                  treeData={this.treeData}
                  autoExpandParent={this.state.autoExpandParent}
                  expandedKeys={this.state.expandedKeys}
                  defaultExpandParent={true}
                  selectedKeys={this.state.selectedKeys}
                  onSelect={this.onSelect}
                  onExpand={this.onExpand}
                />
              </ConfigProvider>
            </div>
          )}
        />
      </div>
    );
  }

  /**
   * Handles tree node selection
   * @param selectedKeys - Array of selected node keys
   */
  onSelect: TreeProps['onSelect'] = (selectedKeys) => {
    if (selectedKeys.length > 0) {
      this.setState({selectedKeys: selectedKeys as string[]});
    }
  };

  /**
   * Recursively searches element tree to find and select node by ID
   * @param selectKey - ID of node to select
   * @param currentElement - Current element in traversal
   */
  onExpand: TreeProps['onExpand'] = (expandedKeys) => {
    this.setState({
      expandedKeys: expandedKeys as string[],
      autoExpandParent: false,
    });
  };

  moveUpSelectNode = (current: ElementTreeDataNode | undefined) => {
    if (!current || !current.parentNode) {
      return;
    }
    const parent = current.parentNode;
    if (parent.children.length > 0) {
      // find the first child node before current node.
      for (let i = 0; i < parent.children.length; i++) {
        if (parent.children[i].key === current.key) {
          if (i > 0) {
            let upNode = parent.children[i - 1];
            while (this.state.expandedKeys.includes(upNode.key)) {
              upNode = upNode.children[upNode.children.length - 1];
            }
            this.setState({selectedKeys: [upNode.key]});
            return;
          } else {
            this.setState({selectedKeys: [parent.key]});
            return;
          }
        }
      }
    }
  };

  moveDownSelectNode = (current: ElementTreeDataNode | undefined) => {
    if (!current) {
      return;
    }
    if (this.state.expandedKeys.includes(current.key)) {
      if (current.children.length > 0) {
        this.setState({selectedKeys: [current.children[0].key]});
        return;
      }
    }
    let parent = current.parentNode;
    while (parent) {
      for (let i = 0; i < parent.children.length; i++) {
        if (parent.children[i].key === current.key) {
          if (i < parent.children.length - 1) {
            this.setState({selectedKeys: [parent.children[i + 1].key]});
            return;
          } else {
            break;
          }
        }
      }
      current = parent;
      parent = parent.parentNode;
    }
  };

  expandCurrentNode = (current: ElementTreeDataNode | undefined) => {
    if (
      !current ||
      this.state.expandedKeys.includes(current.key)
    ) {
      return;
    }
    if (current.children.length <= 0) {
      return;
    }
    this.setState({expandedKeys: [...this.state.expandedKeys, current.key]});
  };

  collapseCurrentNode = (current: ElementTreeDataNode | undefined) => {
    if (
      !current ||
      !this.state.expandedKeys.includes(current.key)
    ) {
      return;
    }
    if (current.children.length <= 0) {
      return;
    }

    const removedExpanedKeys: string[] = [];
    const traverse = (node: ElementTreeDataNode) => {
      if (node.children.length > 0) {
        node.children.forEach((child: ElementTreeDataNode) => {
          traverse(child);
        });
        removedExpanedKeys.push(node.key);
      }
    };
    traverse(current);

    this.setState({
      expandedKeys: this.state.expandedKeys.filter(
        (key) => !removedExpanedKeys.includes(key),
      ),
    });
  };
}
