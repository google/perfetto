// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {Component} from 'react';
import {Button, Select, Space, Typography} from 'antd';
import {lynxPerfGlobals} from '../../../lynx_perf/lynx_perf_globals';
import {LynxViewInstance} from '../../../lynx_perf/types';
import {
  COMMAND_QUERY_LYNX_VIEW,
  NO_INSTANCE_ID,
  PARAMETER_FOCUS_LYNX_VIEWS,
} from '../../../lynx_perf/constants';
import {changeURLParam} from '../../../lynx_perf/url_utils';
import {AppImpl} from '../../../core/app_impl';
import {CheckOutlined, CloseOutlined, CopyOutlined} from '@ant-design/icons';
import {GlobalEventEmitter} from '../../../lynx_perf/common_components/global_event_emitter';


export interface LynxViewInstanceOption {
  label: string;
  value: string;
}

export interface PipelineIdOption {
  label: string;
  value: string;
}


interface State {
  value: string[];
  option: LynxViewInstanceOption[];

  pipelineIdValue: string[];
  pipelineIdOption: PipelineIdOption[];
}

export class FocusLynxViewDetailPanel extends Component<{}, State> {
  constructor() {
    super({});
    this.handleChange = this.handleChange.bind(this);
    this.handleSelectAll = this.handleSelectAll.bind(this);
    this.handleClearAll = this.handleClearAll.bind(this);
    this.handleClose = this.handleClose.bind(this);
    this.initOptions = this.initOptions.bind(this);
    this.state = {
      value: [],
      option: [],
      pipelineIdOption: [],
      pipelineIdValue: [],
    };
  }

  initOptions() {
    if (
      this.state.option.length > 0 ||
      lynxPerfGlobals.state.lynxviewInstances.length <= 0
    ) {
      return;
    }
    const defaultInstanceArray: string[] = [];
    lynxPerfGlobals.state.selectedLynxviewInstances.forEach((item) => {
      defaultInstanceArray.push(item.instanceId);
    });
    const defaultPipelineValue: string[] = [];
    const option = [];
    for (const item of lynxPerfGlobals.state.lynxviewInstances) {
      option.push({
        label: item.url,
        value: item.instanceId,
      });
    }
    this.setState({
      value: defaultInstanceArray,
      option,
      pipelineIdValue: defaultPipelineValue,
    });
  }

  componentDidUpdate(_prevProps: Readonly<{}>, _prevState: Readonly<{}>): void {
    this.initOptions();
  }

  handleChange(value: string[]) {
    const selectedLynxviewInstances: LynxViewInstance[] = [];
    lynxPerfGlobals.state.lynxviewInstances.forEach((item) => {
      if (value.includes(item.instanceId)) {
        selectedLynxviewInstances.push(item);
      }
    });
    this.setState({value: value});
    lynxPerfGlobals.updateSelectedLynxViewInstances(selectedLynxviewInstances);
    lynxPerfGlobals.setHighlightNoInstanceIdTrace(
      selectedLynxviewInstances.some(
        (item) => item.instanceId === NO_INSTANCE_ID,
      ),
    );
    if (value.length <= 0) {
      changeURLParam(PARAMETER_FOCUS_LYNX_VIEWS, '');
    } else {
      changeURLParam(PARAMETER_FOCUS_LYNX_VIEWS, value.join(','));
    }
    if (AppImpl.instance.commands.hasCommand(COMMAND_QUERY_LYNX_VIEW)) {
      AppImpl.instance.commands.runCommand(COMMAND_QUERY_LYNX_VIEW);
    }
  }

  handleSelectAll() {
    this.setState({value: this.state.option.map((item) => item.value)});
    lynxPerfGlobals.updateSelectedLynxViewInstances(
      lynxPerfGlobals.state.lynxviewInstances,
    );
    lynxPerfGlobals.setHighlightNoInstanceIdTrace(true);
    changeURLParam(
      PARAMETER_FOCUS_LYNX_VIEWS,
      this.state.option.map((item) => item.value).join(','),
    );

    if (AppImpl.instance.commands.hasCommand(COMMAND_QUERY_LYNX_VIEW)) {
      AppImpl.instance.commands.runCommand(COMMAND_QUERY_LYNX_VIEW);
    }
  }

  handleClearAll() {
    this.setState({
      value: [],
    });
    changeURLParam(PARAMETER_FOCUS_LYNX_VIEWS, '');
    lynxPerfGlobals.updateSelectedLynxViewInstances([]);
    lynxPerfGlobals.setHighlightNoInstanceIdTrace(false);
    if (AppImpl.instance.commands.hasCommand(COMMAND_QUERY_LYNX_VIEW)) {
      AppImpl.instance.commands.runCommand(COMMAND_QUERY_LYNX_VIEW);
    }
  }

  handleClose() {
    lynxPerfGlobals.closeRightSidebar();
    GlobalEventEmitter.emit('stateChanged', {});
  }

  render() {
    return (
      <div>
        <div className="rightbar-title-container">
          <h1>Focus LynxView</h1>
          <div className="rightbar-close-container">
            <CloseOutlined onClick={this.handleClose} />
          </div>
        </div>

        <div className="rightbar-lynxview-container">
          <Space className="rightbar-buttons">
            <Button type="primary" onClick={this.handleSelectAll} size="small">
              Select All
            </Button>
            <Button onClick={this.handleClearAll} size="small">
              Clear All
            </Button>
          </Space>
          <Select
            mode="multiple"
            allowClear={true}
            placeholder="Select instanceId or search using url"
            onChange={this.handleChange}
            style={{width: '100%'}}
            defaultValue={this.state.value}
            filterOption={(input, option) =>
              (option?.label ?? '').toLowerCase().includes(input.toLowerCase())
            }
            options={this.state.option}
            value={this.state.value}
            optionLabelProp="value"
            listHeight={600}
            optionRender={(option) => (
              <div className="select-scroll-container">
                <Typography.Text>
                  {option.data.value === NO_INSTANCE_ID ? NO_INSTANCE_ID: `instanceId: ${option.data.value}`}
                </Typography.Text>
                {option.data.label && (
                  <CustomCopyableParagraph
                    text={option.data.label}
                    copiedDuration={800}
                  />
                )}
              </div>
            )}
          />
        </div>
      </div>
    );
  }
}

interface CustomCopyableParagraphProps {
  text: string;
  copiedDuration?: number;
  style?: React.CSSProperties;
}
interface CustomCopyableParagraphState {
  copied: boolean;
}
class CustomCopyableParagraph extends Component<
  CustomCopyableParagraphProps,
  CustomCopyableParagraphState
> {
  private timeoutId?: number;
  static defaultProps = {
    copiedDuration: 1000,
  };
  constructor(props: CustomCopyableParagraphProps) {
    super(props);
    this.state = {
      copied: false,
    };
  }
  componentWillUnmount() {
    if (this.timeoutId != undefined) {
      clearTimeout(this.timeoutId);
    }
  }
  handleCopy = (event: React.MouseEvent) => {
    event.stopPropagation();
    navigator.clipboard.writeText(this.props.text).then(() => {
      this.setState({copied: true});
      if (this.timeoutId != undefined) {
        clearTimeout(this.timeoutId);
      }
      this.timeoutId = window.setTimeout(() => {
        this.setState({copied: false});
      }, this.props.copiedDuration);
    });
  };
  render() {
    const {style} = this.props;
    const {copied} = this.state;
    return (
      <Typography.Paragraph style={style}>
        {`url: ${this.props.text}`}
        <span
          onClick={this.handleCopy}
          style={{cursor: 'pointer', userSelect: 'none', marginLeft: '5px'}}
          aria-label="Copy text"
          role="button"
          tabIndex={0}>
          {copied ? (
            <CheckOutlined style={{color: 'green'}} />
          ) : (
            <CopyOutlined style={{color: 'blue'}} />
          )}
        </span>
      </Typography.Paragraph>
    );
  }
}