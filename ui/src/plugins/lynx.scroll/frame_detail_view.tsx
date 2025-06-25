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
import {createRoot, Root} from 'react-dom/client';
import {Component} from 'react';
import {ConfigProvider, Table} from 'antd';
import {TableColumnTitle} from '../../lynx_perf/common_components/table_column_title';
export interface FrameDetailAttr {
  frameItems: FrameItem[];
}

export interface FrameItem {
  type: string;
  averageWallDuration: number;
  occurrence: number;
  percentage: number;
}

export class FrameDetailView implements m.ClassComponent<FrameDetailAttr> {
  private root: Root | undefined;
  oncreate(vnode: m.CVnodeDOM<FrameDetailAttr>) {
    this.root = createRoot(vnode.dom);
    this.root.render(<FrameSectionPanel frameItems={vnode.attrs.frameItems} />);
  }

  view() {
    return m('.pf-section');
  }
}
export class FrameSectionPanel extends Component<FrameDetailAttr> {
  constructor(props: FrameDetailAttr) {
    super(props);
    this.state = {};
  }

  render() {
    const {frameItems} = this.props;
    if (frameItems == null || frameItems.length <= 0) {
      return <div></div>;
    }

    const frameDataSource = frameItems;
    const frameColumns = [
      {
        title: <TableColumnTitle title="Type" />,
        dataIndex: 'type',
        key: 'type',
      },
      {
        title: <TableColumnTitle title="Average duration (ms)" />,
        dataIndex: 'averageWallDuration',
        key: 'averageWallDuration',
      },
      {
        title: <TableColumnTitle title="Occurrences" />,
        dataIndex: 'occurrence',
        key: 'occurrence',
      },
      {
        title: <TableColumnTitle title="Percentage" />,
        dataIndex: 'percentage',
        key: 'percentage',
        render: (value: number, _record: FrameItem) => <div>{value}%</div>,
      },
    ];

    return (
      <div>
        <h1 className="detail-title" style={{margin: 6}}>
          Frame Rendering
        </h1>
        <div style={{width: '100%', height: 1, background: '#0000001a'}}></div>
        <p className="detail-text" style={{margin: 6}}>
          {`The table below lists frame rendering metrics recorded during the
          scoll period.`}{' '}
          <br />
          {`The rendering duration per frame is calculated by summing the execution time of `}
          <span style={{fontWeight: 'bold'}}>Choreographer#doFrame</span>
          {` on the UI thread and `}
          <span style={{fontWeight: 'bold'}}>DrawFrames</span>
          {` on the RenderThread.  When overlapping periods exist between the two threads, the duplicated time is deducted. Color indicates performance levels:`}
        </p>
        <ul className="detail-text custom-scroll-ul">
          <li className="custom-scroll-li">
            <span style={{fontWeight: 'bold'}}>Red:</span> Frame rendering
            duration ≥ 32ms (suboptimal)
          </li>
          <li className="custom-scroll-li">
            <span style={{fontWeight: 'bold'}}>Orange:</span> Frame rendering
            duration between 16ms–32ms (acceptable)
          </li>
          <li className="custom-scroll-li">
            <span style={{fontWeight: 'bold'}}>Green:</span> Frame rendering
            duration &lt; 16ms (ideal)
          </li>
        </ul>
        <ConfigProvider
          theme={{
            token: {
              colorBgContainer: '#ECEFF1',
            },
          }}>
          <Table
            bordered
            rowClassName="table-content-text"
            size="small"
            dataSource={frameDataSource}
            columns={frameColumns}
            pagination={false}
          />
        </ConfigProvider>
      </div>
    );
  }
}
