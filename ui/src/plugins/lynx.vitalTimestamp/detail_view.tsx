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
import {Component, createRef, RefObject} from 'react';
import {Table} from 'antd';
import MithrilReactWrapper from '../../lynx_perf/mithril-react-wrapper';
import {PipelineStage} from './details';
import {DetailsShell} from '../../widgets/details_shell';
import {TableColumnTitle} from '../../lynx_perf/common_components/table_column_title';
import {Icons} from '../../base/semantic_icons';

import {Button} from '../../widgets/button';
import {chart} from '../../metrics_chart';
import {TTraceEvent} from '../../metrics_chart/types';
import {LynxElement} from '../../lynx_perf/common_components/element_tree/types';
import {ElementTreeView} from '../../lynx_perf/common_components/element_tree/element_tree_view';
import {SliceDetails} from '../../components/sql_utils/slice';
import {SliceRef} from '../../components/widgets/slice';
import {CRUCIAL_TIMING_KEYS} from '../../lynx_perf/constants';

export interface VitalTimestampDetailAttr {
  pipelineStagesDetail?: PipelineStage[];
  sliceDetail?: SliceDetails;
  chartEvents: TTraceEvent[];
  elementTree?: LynxElement;
}

export interface VitalTimestampDetailState {
  isContainerActive: boolean;
  showDialog: boolean;
}

/**
 * Vital Timestamp Detail View Component (Mithril)
 * 
 * Acts as a bridge between Mithril and React, rendering the DetailViewPanel
 * React component within a Mithril application context.
 */
export class VitalTimestampDetailView
  implements m.ClassComponent<VitalTimestampDetailAttr>
{
  private root?: Root;

  /**
   * Creates React root when component mounts
   * @param vnode - Mithril virtual node with component attributes
   */
  oncreate(vnode: m.CVnodeDOM<VitalTimestampDetailAttr>) {
    this.root = createRoot(vnode.dom);
    this.render(vnode);
  }

  /**
   * Renders React component into Mithril DOM
   * @param vnode - Mithril virtual node with component attributes
   */
  render(vnode: m.CVnodeDOM<VitalTimestampDetailAttr>) {
    if (this.root != undefined) {
      this.root.render(
        <DetailViewPanel
          pipelineStagesDetail={vnode.attrs.pipelineStagesDetail}
          sliceDetail={vnode.attrs.sliceDetail}
          chartEvents={vnode.attrs.chartEvents}
          elementTree={vnode.attrs.elementTree}
        />
      );
    }
  }

  onupdate(vnode: m.CVnodeDOM<VitalTimestampDetailAttr>) {
    this.render(vnode);
  }

  view() {
    return m('.page');
  }
}

/**
 * Vital Timestamp Detail Panel (React)
 * 
 * Displays detailed performance information including:
 * - Pipeline timing visualization
 * - Stage-by-stage breakdown
 * - Optional element tree inspection
 */
export class DetailViewPanel extends Component<
  VitalTimestampDetailAttr,
  VitalTimestampDetailState
> {
  containerRef: RefObject<HTMLDivElement | null>;
  chartInstance?: {
    close: () => void;
  }
  constructor(props: VitalTimestampDetailAttr) {
    super(props);
    this.state = {
      isContainerActive: false,
      showDialog: false,
    };
    this.containerRef = createRef();
    this.handleResize = this.handleResize.bind(this);
    this.createChartInstance = this.createChartInstance.bind(this);
    this.closeDialog = this.closeDialog.bind(this);
    this.showDialog = this.showDialog.bind(this);
  }

  /**
   * Initializes chart visualization and window resize handler
   */
  componentDidMount() {
    this.createChartInstance();
    window.addEventListener('resize', this.handleResize);
  }

  /**
   * Creates performance timeline chart
   * @remarks Uses the chart utility to visualize pipeline stage timings
   */
  private createChartInstance() {
    if (this.containerRef.current) {
      const data = this.props.chartEvents;
      if (data.length > 0) {
        this.chartInstance = chart({
          container: this.containerRef.current,
          data,
          config: {
            basis: data[0].ts,
            node: {
              margin: 5,
            },
          },
          events: {},
        });
      }
    }
  };

  /**
   * Handles window resize events
   * @remarks Recreates chart to maintain proper dimensions
   */
  private handleResize() {
    if (this.chartInstance != undefined) {
      this.chartInstance.close();
      this.createChartInstance();
    }
  };

  /**
   * Cleans up chart and event listeners
   */
  componentWillUnmount() {
    if (this.chartInstance != undefined) {
      this.chartInstance.close();
    }
    window.removeEventListener('resize', this.handleResize);
  }

  /**
   * Main render method for detail panel
   * @returns React node with three main sections:
   * 1. Header with pipeline stage info
   * 2. Timing visualization chart
   * 3. Detailed stage breakdown table
   */
  render() {
    const {pipelineStagesDetail, sliceDetail} = this.props;
    if (
      !pipelineStagesDetail ||
      pipelineStagesDetail.length <= 0 ||
      !sliceDetail
    ) {
      return <div></div>;
    }
    const firstPipelineStage = pipelineStagesDetail[0].name;
    const lastPipelineStage =
      pipelineStagesDetail[pipelineStagesDetail.length - 1].name;
    const pipelineDuration = (
      pipelineStagesDetail[pipelineStagesDetail.length - 1].dur +
      pipelineStagesDetail[pipelineStagesDetail.length - 1].ts -
      pipelineStagesDetail[0].ts
    ).toFixed(2);

    // stage detail
    const stagDetailDataSource = pipelineStagesDetail;
    const stagDetailColumns = [
      {
        title: <TableColumnTitle title="Name" />,
        dataIndex: 'name',
        key: 'name',
      },
      {
        title: <TableColumnTitle title="Duration(ms)" />,
        dataIndex: 'dur',
        key: 'dur',
        render: (value: number, record: PipelineStage) => (
          <div>{CRUCIAL_TIMING_KEYS.includes(record.name) ? 'NA' : value}</div>
        ),
      },
      {
        title: <TableColumnTitle title="Start slice" />,
        dataIndex: 'id',
        key: 'id',
        render: (value: number, _record: unknown, _index: number) => (
          <MithrilReactWrapper
            component={SliceRef}
            id={value}
            name={value}
            switchToCurrentSelectionTab={false}
          />
        ),
      },
      {
        title: <TableColumnTitle title="End slice" />,
        dataIndex: 'endId',
        key: 'endId',
        render: (value: number, _record: unknown, _index: number) => (
          <MithrilReactWrapper
            component={SliceRef}
            id={value}
            name={value}
            switchToCurrentSelectionTab={false}
          />
        ),
      },
    ];

    return (
      <div>
        <MithrilReactWrapper
          component={DetailsShell}
          title={'Current Pipeline Stage'}
          description={m(
            'div',
            {style: {whiteSpace: 'pre'}},
            `${sliceDetail.name}`,
          )}
          buttons={
            this.props.elementTree
              ? m(Button, {
                  compact: true,
                  label: 'Element Tree',
                  rightIcon: Icons.SortedAsc,
                  onclick: (_e) => {
                    this.showDialog();
                  },
                })
              : null
          }
        />
        <div className="detail-box-container">
          <h1 className="detail-title" style={{marginBottom: 10}}>
            Pipeline Timing Diagram
          </h1>
          <p className="detail-text">
            {
              'The chart below displays the duration of each stage in this pipeline, the 0ms is the beginning of the first pipeline stage. For detailed descriptions of the pipeline stages, please refer to '
            }
            <a
              href="https://lynxjs.org/living-spec/index.html?ts=1742218715202#pipeline"
              target="_blank">
              Lynx Living Spec
            </a>
          </p>
          <div
            ref={this.containerRef}
            style={{
              width: '100%',
            }}
          />
        </div>
        <div className="detail-box-container">
          <h1 className="detail-title" style={{marginBottom: 10}}>
            Pipeline Detail
          </h1>
          <p className="detail-text">
            {`The duration of the pipeline from '${firstPipelineStage}' to '${lastPipelineStage}' is ${pipelineDuration} milliseconds`}
          </p>
          <Table
            bordered
            rowClassName="table-content-text"
            size="small"
            dataSource={stagDetailDataSource}
            columns={stagDetailColumns}
            pagination={false}
          />
        </div>

        {this.state.showDialog && (
          <ElementTreeView
            selectedElement={this.props.elementTree}
            rootElement={this.props.elementTree}
            closeDialog={this.closeDialog}
          />
        )}
      </div>
    );
  }

  /**
   * Closes element tree inspection dialog
   */
  private closeDialog() {
    this.setState({showDialog: false});
  }

  /**
   * Opens element tree inspection dialog
   */
  private showDialog() {
    this.setState({showDialog: true});
  }
}
