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

import m from 'mithril';
import {createRoot} from 'react-dom/client';
import {
  Table,
  Modal,
  Tree,
  TreeDataNode,
  TreeProps,
  Tooltip,
  ConfigProvider,
} from 'antd';
import Icon, {QuestionCircleOutlined} from '@ant-design/icons';
import {Component, createRef, RefObject} from 'react';
import {
  ElementDetailAttr,
  ElementState,
  ElementTreeViewProps,
  ElementTreeViewState,
  IssuseElements,
  LynxElement,
} from './types';
import {
  constructElementDetail,
  constructElementDetailWithinDepth,
} from './utils';
import {TableColumnTitle} from '../../frontend/lynx_perf/common_components/table_column_title';
import {lynxConfigState} from '../../lynx_features_flags/config';

export class ElementDetailView implements m.ClassComponent<ElementDetailAttr> {
  oncreate(vnode: m.CVnodeDOM<ElementDetailAttr>) {
    const root = createRoot(vnode.dom);
    root.render(<DetailViewPanel details={vnode.attrs.details} />);
  }

  onupdate(vnode: m.CVnodeDOM<ElementDetailAttr>) {
    const root = createRoot(vnode.dom);
    root.render(<DetailViewPanel details={vnode.attrs.details} />);
  }

  view() {
    return m('.page');
  }
}

export class DetailViewPanel extends Component<
  ElementDetailAttr,
  ElementState
> {
  constructor(props: ElementDetailAttr) {
    super(props);
    this.state = {
      showDialog: false,
      selectedElement: undefined,
    };
  }

  render() {
    const issuesElements: IssuseElements[] = [];
    this.assembleExcessiveNonRenderingElements(issuesElements);
    this.assembleInvisibleElements(issuesElements);
    this.assembleDeeplyNestedElements(issuesElements);

    return (
      <div>
        {issuesElements.map((element, index) => (
          <div key={index} className="detail-box-container">
            <h1 className="detail-title">{element.title}</h1>
            <p className="detail-text">{element.description}</p>
            <Table
              bordered
              style={{marginTop: 20}}
              rowClassName="table-content-text"
              dataSource={element.dataSource}
              columns={element.columns}
              expandable={{
                showExpandColumn: false,
              }}
              pagination={{
                position: ['bottomLeft'],
                hideOnSinglePage: true,
              }}
            />
          </div>
        ))}

        {this.state.showDialog && (
          <ElementTreeView
            selectedElement={this.state.selectedElement}
            rootElement={this.state.selectedElement?.rootElement}
            closeDialog={this.closeDialog}
          />
        )}
      </div>
    );
  }

  private assembleDeeplyNestedElements(issuesElements: IssuseElements[]) {
    const {details} = this.props;
    const deeplyNestedElements = details.filter((item) => item.deeplyNested);
    const deeplyNestedDataSource: LynxElement[] = [];
    for (const item of deeplyNestedElements) {
      deeplyNestedDataSource.push(item);
    }
    if (deeplyNestedDataSource.length > 0) {
      issuesElements.push({
        title: 'Element With Excessive Depth',
        description: (
          <>
            To optimize element hierarchy depth, remove redundant {'<view>'},{' '}
            {'<wrapper>'} and {'<component>'} element in the ancestor chain of
            the current element.
          </>
        ),
        dataSource: deeplyNestedDataSource,
        columns: [
          {
            title: this.renderElementTitle(),
            dataIndex: 'name',
            key: 'name',
            render: this.renderElementRow(),
          },
          {
            title: (
              <TableColumnTitle title="Depth of the element in the element tree" />
            ),
            dataIndex: 'depth',
            key: 'depth',
          },
        ],
      });
    }
  }

  private assembleInvisibleElements(issuesElements: IssuseElements[]) {
    const {details} = this.props;
    const invisibleElements = details.filter((item) => item.invisible);
    const invisibleDataSource: LynxElement[] = [];
    for (const item of invisibleElements) {
      invisibleDataSource.push(item);
    }
    if (invisibleDataSource.length > 0) {
      issuesElements.push({
        title: 'Non-Visible Element Warning',
        description: (
          <>
            Although the element is not visible to users, it still consumes
            rendering resources. Apply a lazy-loading mechanism to load the
            element on-demand.
            {
              lynxConfigState.state.lynxLazyLoadingUrl &&
              <>
                For implementation guidance, see{' '}
                <a href={lynxConfigState.state.lynxLazyLoadingUrl} target="_blank">
                  Lazy Loading Documentation
                </a>
                .
              </>
            }
          </>
        ),
        dataSource: invisibleDataSource,
        columns: [
          {
            title: this.renderElementTitle(),
            dataIndex: 'name',
            key: 'name',
            render: this.renderElementRow(),
          },
          {
            title: (
              <div style={{display: 'flex'}} className="table-header-text">
                <div style={{marginRight: 5}}>Descendants count</div>
                <Tooltip
                  title="This metric represents the total number of nodes under a specific parent element, including all direct children and subsequent layers of nested elements."
                  color="#00000099">
                  <Icon component={QuestionCircleOutlined} />
                </Tooltip>
              </div>
            ),
            dataIndex: 'descendantCount',
            key: 'descendantCount',
          },
        ],
      });
    }
  }

  private assembleExcessiveNonRenderingElements(
    issuesElements: IssuseElements[],
  ) {
    const {details} = this.props;
    const excessiveNonRenderingElements = details.filter(
      (item) => item.hasExcessiveNonRenderingElements,
    );
    const excessiveNonRenderingDataSource: LynxElement[] = [];
    for (const item of excessiveNonRenderingElements) {
      excessiveNonRenderingDataSource.push(item);
    }
    if (excessiveNonRenderingDataSource.length > 0) {
      issuesElements.push({
        title: 'Excessive Non-Rendering  Elements',
        description: (
          <>
            The element includes an excessive number of non-rendering elements.
            To improve rendering performance, consider removing redundant
            elements such as {'<view>'}, {'<wrapper>'} and {'<component>'}.
          </>
        ),
        dataSource: excessiveNonRenderingDataSource,
        columns: [
          {
            title: this.renderElementTitle(),
            dataIndex: 'name',
            key: 'name',
            render: this.renderElementRow(),
          },
          {
            title: (
              <div style={{display: 'flex'}} className="table-header-text">
                <div style={{marginRight: 5}}>Non-Rendering Elements Ratio</div>
                <Tooltip
                  title="Non-Rendering Elements Ratio = (Σ<component>,<view>,<wrapper> elements in subtree) / (Total subtree elements)"
                  color="#00000099">
                  <Icon component={QuestionCircleOutlined} />
                </Tooltip>
              </div>
            ),
            dataIndex: 'overNoRenderingRatio',
            key: 'overNoRenderingRatio',
            render: (_value: unknown, record: LynxElement) => (
              <div>
                {record.overNoRenderingRatio}({record.wrapDescendantCount}/
                {record.descendantCount})
              </div>
            ),
          },
        ],
      });
    }
  }

  private renderElementTitle() {
    return (
      <div style={{display: 'flex'}} className="table-header-text">
        <div style={{marginRight: 5}}>Element</div>
        <Tooltip
          title="Click the link below to view the element's position within the tree."
          color="#00000099">
          <Icon component={QuestionCircleOutlined} />
        </Tooltip>
      </div>
    );
  }

  private renderElementRow() {
    return (
      // @ts-ignore
      value: unknown,
      record: LynxElement,
    ) => (
      <a
        className="pf-anchor"
        onClick={() => {
          this.setState({showDialog: true, selectedElement: record});
        }}>
        {constructElementDetailWithinDepth(record, 0)}
      </a>
    );
  }

  closeDialog = () => {
    this.setState({showDialog: false});
  };
}

class ElementTreeView extends Component<
  ElementTreeViewProps,
  ElementTreeViewState
> {
  private treeData: TreeDataNode[];
  private containerRef: RefObject<HTMLDivElement | null>;
  constructor(props: ElementTreeViewProps) {
    super(props);
    this.state = {
      currentSelectedElement: props.selectedElement,
      treeHeight: window.innerHeight - 100,
      treeWidth: window.innerWidth - 200,
    };
    this.treeData = [];
    if (props.rootElement) {
      this.treeData = this.constructTreeData(props.rootElement);
    }
    this.containerRef = createRef();
  }

  updateTreeSize = () => {
    this.setState({
      treeHeight: window.innerHeight - 100,
      treeWidth: window.innerWidth - 200,
    });
  };

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

  onSelect: TreeProps['onSelect'] = (selectedKeys) => {
    if (selectedKeys.length > 0) {
      const selectKey = selectedKeys[0] as string;
      // traversal the rootElement, select the element node according to id.
      this.traversalTreeAndSelectElementNode(selectKey, this.props.rootElement);
    }
  };

  traversalTreeAndSelectElementNode = (
    selectKey: string,
    currentElement: LynxElement | undefined,
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
