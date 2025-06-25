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
import {Table, Tooltip} from 'antd';
import Icon, {QuestionCircleOutlined} from '@ant-design/icons';
import {Component} from 'react';
import {lynxConfigState} from '../../lynx_features_flags/config';
import {TableColumnTitle} from '../../lynx_perf/common_components/table_column_title';
import {ElementDetailAttr, ElementState, IssuseElements} from './types';
import {ElementTreeView} from '../../lynx_perf/common_components/element_tree/element_tree_view';
import {LynxElement} from '../../lynx_perf/common_components/element_tree/types';
import {constructElementDetailWithinDepth} from '../../lynx_perf/common_components/element_tree/utils';

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
            {lynxConfigState.state.lynxLazyLoadingUrl && (
              <>
                For implementation guidance, see{' '}
                <a
                  href={lynxConfigState.state.lynxLazyLoadingUrl}
                  target="_blank">
                  Lazy Loading Documentation
                </a>
                .
              </>
            )}
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
      _value: unknown,
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
