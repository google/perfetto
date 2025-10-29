// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import { Component } from 'react';
import { Button, Modal, Form, Input, Select, message } from 'antd';
import { SettingOutlined } from '@ant-design/icons';
import AIAnalysis from '../../../../plugins/lynx.AIAnalysis';

const { Option } = Select;

interface SettingsButtonProps {
  onValidationComplete?: () => Promise<void>;
}

interface SettingsButtonState {
  showSettingsModal: boolean;
  llmConfig: {
    baseUrl: string;
    apiKey: string;
    modelName: string;
    modelProvider: string;
    customPrompt: string;
  };
}

export class SettingsButton extends Component<SettingsButtonProps, SettingsButtonState> {
  constructor(props: SettingsButtonProps) {
    super(props);
    this.state = {
      showSettingsModal: false,
      llmConfig: {
        baseUrl: '',
        apiKey: '',
        modelName: '',
        modelProvider: '',
        customPrompt: ''
      }
    };
  }

  showSettings = () => {
    this.setState({
      showSettingsModal: true,
      llmConfig: {
        baseUrl: AIAnalysis.baseUrlSetting.get() || '',
        apiKey: AIAnalysis.APIKeySetting.get() || '',
        modelName: AIAnalysis.modelNameSetting.get() || '',
        modelProvider: AIAnalysis.modelProviderSetting.get() || '',
        customPrompt: AIAnalysis.customPromptSetting.get() || ''
      }
    });
  };

  hideSettings = () => {
    this.setState({ showSettingsModal: false });
  };

  saveSettings = async () => {
    const { llmConfig } = this.state;
    AIAnalysis.baseUrlSetting.set(llmConfig.baseUrl);
    AIAnalysis.APIKeySetting.set(llmConfig.apiKey);
    AIAnalysis.modelNameSetting.set(llmConfig.modelName);
    AIAnalysis.modelProviderSetting.set(llmConfig.modelProvider);
    AIAnalysis.customPromptSetting.set(llmConfig.customPrompt);
    
    message.success('Save Settings Successfully');
    this.hideSettings();
    
    if (this.props.onValidationComplete) {
      await this.props.onValidationComplete();
    }
  };

  updateLLMConfig = (field: string, value: string) => {
    this.setState({
      llmConfig: {
        ...this.state.llmConfig,
        [field]: value
      }
    });
  };

  render() {
    const { showSettingsModal, llmConfig } = this.state;

    return (
      <>
        <div style={{ position: 'absolute', top: '16px', right: '16px' }}>
          <Button
            type="text"
            icon={<SettingOutlined />}
            onClick={this.showSettings}
            style={{ 
              fontSize: '14px',
              color: '#8c8c8c'
            }}
          >
            Settings
          </Button>
        </div>
        
        <Modal
          title="LLM Configuration"
          open={showSettingsModal}
          onOk={this.saveSettings}
          onCancel={this.hideSettings}
          width={600}
          okText="Save"
          cancelText="Cancel"
        >
          <Form layout="vertical" style={{ marginTop: '16px' }}>
            <Form.Item label="Model Provider">
              <Select
                value={llmConfig.modelProvider || undefined}
                onChange={(value) => this.updateLLMConfig('modelProvider', value)}
                placeholder="Select model provider"
              >
                <Option value="doubao">Doubao</Option>
                <Option value="deepseek">Deepseek</Option>
                <Option value="gemini">Google Gemini</Option>
              </Select>
            </Form.Item>
            
            <Form.Item label="Model Name">
              <Input
                value={llmConfig.modelName}
                onChange={(e) => this.updateLLMConfig('modelName', e.target.value)}
                placeholder="e.g., seed-1.6, deepseek, gemini-2.5-pro"
              />
            </Form.Item>
            
            <Form.Item label="API Key">
              <Input.Password
                value={llmConfig.apiKey}
                onChange={(e) => this.updateLLMConfig('apiKey', e.target.value)}
                placeholder="Enter your API key"
              />
            </Form.Item>
            
            <Form.Item label="Base URL (Optional)">
              <Input
                value={llmConfig.baseUrl}
                onChange={(e) => this.updateLLMConfig('baseUrl', e.target.value)}
                placeholder="e.g., https://ark.cn-beijing.volces.com/api/v3"
              />
            </Form.Item>
            
            <Form.Item label="Custom Prompt (Optional)">
              <Input.TextArea
                value={llmConfig.customPrompt}
                onChange={(e) => this.updateLLMConfig('customPrompt', e.target.value)}
                placeholder="Enter your custom analysis prompt to provide the AI with additional context about the current Trace, such as custom trace events and descriptions."
                rows={5}
              />
            </Form.Item>
          </Form>
        </Modal>
      </>
    );
  }
}