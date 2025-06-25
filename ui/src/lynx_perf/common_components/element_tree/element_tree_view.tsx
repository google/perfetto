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
  private treeData: TreeDataNode[] = [];
  
  /**
   * Reference to the container div for scroll management
   */
  private containerRef: RefObject<HTMLDivElement | null>;
  constructor(props: ElementTreeViewProps) {
    super(props);
    this.state = {
      currentSelectedElement: props.selectedElement,
      treeHeight: window.innerHeight - 100,
      treeWidth: window.innerWidth - 200,
    };
    if (props.rootElement) {
      this.treeData = this.constructTreeData(props.rootElement);
    }
    this.containerRef = createRef();
  }

  /**
   * Updates tree dimensions based on window size
   */
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
  constructTreeData = (rootElement: LynxElement) => {
    function constructTreeDataRecursively(
      current: LynxElement,
    ): TreeDataNode | undefined {
      const currentTree: TreeDataNode = {
        title: constructElementDetail(current),
        key: current.id.toString(),
        children: [],
      };
      for (let i = 0; i < current.children.length; i++) {
        const child = constructTreeDataRecursively(current.children[i]);
        if (child) {
          currentTree.children?.push(child);
        }
      }
      return currentTree;
    }

    const treeData: TreeDataNode[] = [];
    const rootTree = constructTreeDataRecursively(rootElement);
    if (!rootTree) {
      return treeData;
    }
    treeData.push(rootTree);
    return treeData;
  };

  componentDidMount() {
    const treeElement = this.containerRef.current?.querySelector(
      `.ant-tree-node-selected`,
    );
    if (treeElement) {
      treeElement.scrollIntoView({
        behavior: 'smooth',
        block: 'center',
      });
    }
    window.addEventListener('resize', this.updateTreeSize);
  }

  componentWillUnmount() {
    window.removeEventListener('resize', this.updateTreeSize);
  }

  render() {
    if (
      !this.props.rootElement ||
      !this.props.selectedElement ||
      !this.state.currentSelectedElement
    ) {
      return <div></div>;
    }

    const defaultSelectedKeys = [
      this.state.currentSelectedElement.id.toString(),
    ];

    return (
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
                defaultSelectedKeys={defaultSelectedKeys}
                defaultExpandedKeys={defaultSelectedKeys}
                treeData={this.treeData}
                defaultExpandParent={true}
                autoExpandParent={true}
                onSelect={this.onSelect}
              />
            </ConfigProvider>
          </div>
        )}
      />
    );
  }

  /**
   * Handles tree node selection
   * @param selectedKeys - Array of selected node keys
   */
  onSelect: TreeProps['onSelect'] = (selectedKeys) => {
    if (selectedKeys.length > 0) {
      const selectKey = selectedKeys[0] as string;
      // traversal the rootElement, select the element node according to id.
      this.traversalTreeAndSelectElementNode(selectKey, this.props.rootElement);
    }
  };

  /**
   * Recursively searches element tree to find and select node by ID
   * @param selectKey - ID of node to select
   * @param currentElement - Current element in traversal
   */
  traversalTreeAndSelectElementNode = (
    selectKey: string,
    currentElement: LynxElement | undefined
  ) => {
    if (!currentElement) {
      return;
    }
    if (currentElement.id.toString() === selectKey) {
      this.setState({
        currentSelectedElement: currentElement,
      });
      return;
    }
    if (currentElement.children.length > 0) {
      for (const child of currentElement.children) {
        this.traversalTreeAndSelectElementNode(selectKey, child);
      }
    }
  };
}
