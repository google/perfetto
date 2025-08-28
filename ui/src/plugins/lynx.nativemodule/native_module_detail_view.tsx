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
import {NativeModuleSection} from './types';
import {SECTION_COLOR} from './types';
import {TableColumnTitle} from '../../lynx_perf/common_components/table_column_title';
import {SliceDetails} from '../../components/sql_utils/slice';
import {THREAD_UNKNOWN} from '../../lynx_perf/constants';

export interface NativeModuleDetailAttr {
  sectionDetail: NativeModuleSection[] | undefined;
  sliceDetail: SliceDetails | undefined;
}


const AWATIMING_CALLBACK_STAGE_DOTTED_IMAGE = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABQAAAAeCAYAAAAsEj5rAAAABGdBTUEAALGPC/xhBQAAACBjSFJNAAB6JgAAgIQAAPoAAACA6AAAdTAAAOpgAAA6mAAAF3CculE8AAAAomVYSWZNTQAqAAAACAAFARIAAwAAAAEAAQAAARoABQAAAAEAAABKARsABQAAAAEAAABSASgAAwAAAAEAAgAAh2kABAAAAAEAAABaAAAAAAAAAJAAAAABAAAAkAAAAAEABJKGAAcAAAASAAAAkKABAAMAAAABAAEAAKACAAQAAAABAAAAFKADAAQAAAABAAAAHgAAAABBU0NJSQAAAFNjcmVlbnNob3R+Qvo8AAAACXBIWXMAABYlAAAWJQFJUiTwAAADBGlUWHRYTUw6Y29tLmFkb2JlLnhtcAAAAAAAPHg6eG1wbWV0YSB4bWxuczp4PSJhZG9iZTpuczptZXRhLyIgeDp4bXB0az0iWE1QIENvcmUgNi4wLjAiPgogICA8cmRmOlJERiB4bWxuczpyZGY9Imh0dHA6Ly93d3cudzMub3JnLzE5OTkvMDIvMjItcmRmLXN5bnRheC1ucyMiPgogICAgICA8cmRmOkRlc2NyaXB0aW9uIHJkZjphYm91dD0iIgogICAgICAgICAgICB4bWxuczpleGlmPSJodHRwOi8vbnMuYWRvYmUuY29tL2V4aWYvMS4wLyIKICAgICAgICAgICAgeG1sbnM6dGlmZj0iaHR0cDovL25zLmFkb2JlLmNvbS90aWZmLzEuMC8iPgogICAgICAgICA8ZXhpZjpQaXhlbFhEaW1lbnNpb24+MTY4PC9leGlmOlBpeGVsWERpbWVuc2lvbj4KICAgICAgICAgPGV4aWY6Q29sb3JTcGFjZT4xPC9leGlmOkNvbG9yU3BhY2U+CiAgICAgICAgIDxleGlmOlVzZXJDb21tZW50PlNjcmVlbnNob3Q8L2V4aWY6VXNlckNvbW1lbnQ+CiAgICAgICAgIDxleGlmOlBpeGVsWURpbWVuc2lvbj45MjwvZXhpZjpQaXhlbFlEaW1lbnNpb24+CiAgICAgICAgIDx0aWZmOlJlc29sdXRpb25Vbml0PjI8L3RpZmY6UmVzb2x1dGlvblVuaXQ+CiAgICAgICAgIDx0aWZmOllSZXNvbHV0aW9uPjE0NDwvdGlmZjpZUmVzb2x1dGlvbj4KICAgICAgICAgPHRpZmY6WFJlc29sdXRpb24+MTQ0PC90aWZmOlhSZXNvbHV0aW9uPgogICAgICAgICA8dGlmZjpPcmllbnRhdGlvbj4xPC90aWZmOk9yaWVudGF0aW9uPgogICAgICA8L3JkZjpEZXNjcmlwdGlvbj4KICAgPC9yZGY6UkRGPgo8L3g6eG1wbWV0YT4KE9AuzQAAAUxJREFUSA3tlttqhDAQhicxHkC9EPH9H1BEEBVBcds/ZdpkN4kuTe8aCLvO4TOTzEwU0zQ9KDDGcdTauq5JKRWw/FIFLQDDPI5Dz6ZpLqHS90oTBpvnZ5+fE+hyLsuSrsJeloVegD5Y27ZUFIVvYQTYMAw28Lewbdt+gDFgWL4OORZMA2PCcHASQOQZDwjvHgD2jAf7yXdhDDB/GYYsUFVV0TzPZApNY9d/2GIgTZIksSISn8LHuq6U53kwz1zgfd9JCGGVo7hqDi5QSPZSKSHjO7p/4J1dCtv83R4ip8yqCa/D1qIXchnqO4Wb43me1HWdrhrbxf/EzQWFgR6gGMZv6Ptee3N5+VH2PcPRSdQjw+CcpqmuzxAIOl4ZgyDLsoykCbvbIFwwXLGY36ccA4YPAQ2MBUPYMiZMA6/aPYxCe/b8vSNDl/e7MNh/AM36Jw58d0vHAAAAAElFTkSuQmCC';
export class NativeModuleDetailView
  implements m.ClassComponent<NativeModuleDetailAttr>
{
  private root: Root | undefined;
  oncreate(vnode: m.CVnodeDOM<NativeModuleDetailAttr>) {
    this.root = createRoot(vnode.dom);
    this.root.render(
      <DetailViewPanel
        sectionDetail={vnode.attrs.sectionDetail}
        sliceDetail={vnode.attrs.sliceDetail}
      />,
    );
  }

  view() {
    return m('.pf-section');
  }
}
export class DetailViewPanel extends Component<NativeModuleDetailAttr> {
  constructor(props: NativeModuleDetailAttr) {
    super(props);
    this.state = {};
  }

  private formatToMs(time: number) {
    return (time / 1000000).toFixed(2);
  }

  private getThreadDescription(section: NativeModuleSection) {
    if (typeof section.thread === 'string') {
      return section.thread;
    }
    return Object.entries(section.thread)
      .map(([key, value]) => `${key}: ${this.formatToMs(value)}ms`)
      .join('\n');
  }

  private awatingCallbackStage(thread: string, sectionIndex: number) {
    return thread === THREAD_UNKNOWN  && sectionIndex === 2;
  }

  render() {
    const {sectionDetail, sliceDetail} = this.props;
    if (!sectionDetail || !sliceDetail) {
      return <div></div>;
    }

    // stage detail
    const stagDetailDataSource = sectionDetail.map((item) => ({
      duration: this.formatToMs(item.endTs - item.beginTs),
      name: item.name,
      description: item.description,
      thread: this.getThreadDescription(item),
    }));
    const stagDetailColumns = [
      {
        title: <TableColumnTitle title="Stage" />,
        dataIndex: 'name',
        key: 'name',
        width: 150,
        render: (value: string, record: typeof stagDetailDataSource[0], index: number) => (
          <div style={{display: 'flex', alignItems: 'center'}}>
            {!this.awatingCallbackStage(record.thread, index) && 
              <div
              style={{
                width: 10,
                height: 15,
                backgroundColor: SECTION_COLOR[index],
                marginRight: 5,
                flexShrink: 0,
              }}>
              </div>
            }
            {
              this.awatingCallbackStage(record.thread, index) && 
              <img src={AWATIMING_CALLBACK_STAGE_DOTTED_IMAGE} style={{width:10, height:15, marginRight: 5,
                flexShrink: 0,}}/>
            }
            <div>{value}</div>
          </div>
        ),
      },
      {
        title: <TableColumnTitle title="Duration(ms)" />,
        dataIndex: 'duration',
        key: 'duration',
      },
      {
        title: <TableColumnTitle title="Running thread" />,
        dataIndex: 'thread',
        key: 'thread',
        width: 120,
        render: (value: string) => (
          <div>
            {value.split('\n').map((line, index) => (
              <div key={index}>
                {line}
                <br />
              </div>
            ))}
          </div>
        ),
      },
      {
        title: <TableColumnTitle title="Description" />,
        dataIndex: 'description',
        key: 'description',
      },
    ];

    return (
      <div>
        <h1 className="detail-title" style={{margin: 6}}>
          Stages
        </h1>
        <p className="detail-text" style={{marginLeft: 6}}>
          {`A single NativeModule call primarily goes through the following five key stages, which may be executed across multiple threads. For more detailed information, you can click the 'Original Slice' button in the upper right corner.`}
        </p>
        <ConfigProvider
          theme={{
            token: {
              colorBgContainer: '#ffffff',
            },
          }}>
          <Table
            bordered
            rowClassName="table-content-text"
            size="small"
            dataSource={stagDetailDataSource}
            columns={stagDetailColumns}
            pagination={false}
          />
        </ConfigProvider>
      </div>
    );
  }
}
