// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import { Component } from 'react';
import { Button } from 'antd';
import {AnalysisProcess} from './ai_analysis/analysis_process';
import {AnalysisReportComponent} from './ai_analysis/analysis_report';
import {SettingsButton} from './ai_analysis/settings_button';
import { AppImpl } from '../../../core/app_impl';
import AIAnalysis from '../../../plugins/lynx.AIAnalysis';
import { AnalysisReport, AnalysisStep, llmState, StepListener } from '../../../lynx_perf/llm_state';
import { STR } from '../../../trace_processor/query_result';
import { eventLoggerState } from '../../../event_logger';


export interface TraceAssistantPanelState {
  status: 'initial' | 'analyzing' | 'completed';
  analysisResult: string;
  extraActionArea?: React.ReactNode;
  extraActionProperties: Record<string, string>;
  analysisSteps: AnalysisStep[];
  validationError: string;
  isValidationPassed: boolean;
}


export class TraceAssistantPanel extends Component<{}, TraceAssistantPanelState> implements StepListener {
  constructor(props: {}) {
    super(props);
    this.state = {
      status: 'initial',
      analysisResult: '',
      extraActionArea: undefined,
      extraActionProperties: {},
      analysisSteps: [],
      validationError: '',
      isValidationPassed: false
    };
  }

  async componentDidMount() {
    await this.performValidation();
    await this.restorePrevReportStatus();
  }

  performValidation = async () => {
    const isLynxVersionValid = await this.validateLynxVersion();
    if (!isLynxVersionValid) {
      this.setState({
        validationError: 'Use Lynx SDK version 3.4 or above to enable AI analysis.',
        isValidationPassed: false
      });
      eventLoggerState.state.eventLogger.logEvent('ai_analysis_low_lynx_version', {});
      return;
    }

    const isLLMConfigValid = this.validateLLMConfig();
    if (!isLLMConfigValid) {
      this.setState({
        validationError: 'Add LLM configuration in Settings to enable AI analysis.',
        isValidationPassed: false
      });
      return;
    }

    this.setState({
      validationError: '',
      isValidationPassed: true
    });
  };

  restorePrevReportStatus = async () => {
    try {
      const prevAnalysisResult = await llmState.state.reportExtraAction?.getHistoryAnalysisReport();
      if (prevAnalysisResult && this.state.status == 'initial') {
        console.log('prevAnalysisResult', JSON.stringify(prevAnalysisResult));
        const extraActionArea = await llmState.state.reportExtraAction?.render(undefined, undefined, prevAnalysisResult.extraActionProperties);
        this.setState({
          ...this.state,
          status: 'completed',
          analysisResult: prevAnalysisResult.analysisResult,
          analysisSteps: prevAnalysisResult.analysisSteps,
          extraActionArea: extraActionArea,
          extraActionProperties: prevAnalysisResult.extraActionProperties,
        });
      }
    } catch (error) {
      console.error('restore prev report status failed : ', error);
    }
  };

  saveCurrentReportStatus = async () => {
    const { analysisResult, analysisSteps, extraActionProperties } = this.state;
    const report: AnalysisReport = {
      analysisResult,
      analysisSteps,
      extraActionProperties,
    };
    await llmState.state.reportExtraAction?.saveAnalysisReport(report);
  };

  private getLLMConfig = () => {
    const modelProvider = AIAnalysis.modelProviderSetting.get();
    const modelName = AIAnalysis.modelNameSetting.get();
    const apiKey = AIAnalysis.APIKeySetting.get();
    const baseUrl = AIAnalysis.baseUrlSetting.get();
    // if llm config is not set through settings page, use default config
    if (!modelProvider && !modelName && !apiKey && !baseUrl) {
      return llmState.state.config;
    }
    return {
      modelProvider,
      modelName,
      apiKey,
      baseUrl
    };
  }

  onStepUpdate = (stepId: string, title: string, status: 'wait' | 'process' | 'finish' | 'error', content: string) => {
    this.setState(prevState => {
      const existingStepIndex = prevState.analysisSteps.findIndex(step => 
        step.id === stepId || step.title.toLowerCase().includes(stepId.toLowerCase())
      );
      
      if (existingStepIndex !== -1) {
        // update current step
        const updatedSteps = [...prevState.analysisSteps];
        updatedSteps[existingStepIndex] = {
          ...updatedSteps[existingStepIndex],
          status,
          details: content ? [...updatedSteps[existingStepIndex].details, content] : updatedSteps[existingStepIndex].details
        };
        return { analysisSteps: updatedSteps };
      } else {
        // add new step
        const newStep: AnalysisStep = {
          id: stepId,
          title: title,
          status: status,
          details: content ? [content] : [],
          collapsed: false
        };
        return { analysisSteps: [...prevState.analysisSteps, newStep] };
      }
    });
  };

  private triggerTraceAIAnalysis = async (from: string) => {
    eventLoggerState.state.eventLogger.logEvent('ai_analysis_click', {
      from: from
    });
    this.setState({
      status: 'analyzing',
    });
    try {
      const report = await llmState.state.traceAnalysis?.analysis(this);
      if (report) {
        this.setState({
          status: 'completed',
          analysisResult: report.analysisResult,
          analysisSteps: report.analysisSteps,
          extraActionArea: report.extraActionArea,
          extraActionProperties: report.extraActionProperties,
        }, async () => {
          await this.saveCurrentReportStatus();
        });
        eventLoggerState.state.eventLogger.logEvent('ai_analysis_show_report', {});
        return;
      }
    } catch (error) {
      console.error('AI analysis request failed:', error);
      this.setState({
        status: 'completed',
        analysisResult: 'Analysis failed, please try again later.',
        extraActionArea: undefined
      });
    }
  };

  validateLynxVersion = async (): Promise<boolean> => {
     const engine = AppImpl.instance.trace?.engine;
    if (!engine) {
      return true;
    }
    const result = await engine.query(`select args.display_value from slice join args on args.arg_set_id=slice.arg_set_id where slice.name='LynxEngineVersion' and args.key='debug.version'`);
    const version = result.numRows() > 0 ? result.firstRow({display_value: STR}).display_value : '';
    return version >= '3.4';
  };

  validateLLMConfig = (): boolean => {
    const config = this.getLLMConfig();
    return !!(config.apiKey && config.modelName && config.modelProvider);
  };

  startAnalysis = async () => {
    this.triggerTraceAIAnalysis("home");
  };

  restartAnalysis = () => {
    this.resetToInitial();
    this.triggerTraceAIAnalysis("restart");
  };

  resetToInitial = () => {
    this.setState({
      status: 'initial',
      analysisResult: '',
      extraActionArea: undefined,
      extraActionProperties: {},
      analysisSteps: []
    });
  };

  renderContent = () => {
    const { status } = this.state;

    switch (status) {
      case 'initial':
        return (
          <div style={{ 
            padding: '24px', 
            textAlign: 'center',
            height: '100%',
            display: 'flex',
            flexDirection: 'column',
            justifyContent: 'center',
            backgroundColor: '#fafafa'
          }}>
            <div style={{ marginBottom: '32px' }}>
              <h2 style={{ 
                fontSize: '24px', 
                fontWeight: '600', 
                color: '#262626',
                marginBottom: '16px'
              }}>
                AI Trace Analysis
              </h2>
              <p style={{ 
                fontSize: '14px', 
                color: '#8c8c8c',
                lineHeight: '1.5'
              }}>
                Analyze your trace with AI to identify performance bottlenecks and optimization opportunities
              </p>
            </div>
            
            <div style={{ marginBottom: '24px' }}>
              <Button
                type="primary"
                size="large"
                onClick={this.startAnalysis}
                disabled={!this.state.isValidationPassed}
                style={{ 
                  height: '48px',
                  fontSize: '16px',
                  fontWeight: '500',
                  borderRadius: '6px',
                  minWidth: '160px'
                }}
              >
                Start Analysis
              </Button>
              
              {this.state.validationError && (
                <div style={{
                  marginTop: '12px',
                  color: '#ff4d4f',
                  fontSize: '14px',
                  textAlign: 'center'
                }}>
                  {this.state.validationError}
                </div>
              )}
            </div>
            
            <SettingsButton onValidationComplete={this.performValidation} />
          </div>
        );

      case 'analyzing':
        return (
           <div style={{ height: '100%', overflowY: 'auto', padding: '16px', position: 'relative' }}>
               <div style={{ marginTop: '24px' }}>
                  <AnalysisProcess steps={this.state.analysisSteps} />
               </div>
               <SettingsButton onValidationComplete={this.performValidation} />
           </div>
        )
      case 'completed':
        return (
          <div style={{ height: '100%', overflowY: 'auto', padding: '16px', position: 'relative' }}>
            <div style={{ marginBottom: '24px', marginTop: '24px' }}>
              <AnalysisProcess steps={this.state.analysisSteps} />
            </div>
            
            {/* Report */}
            <div style={{ marginBottom: '24px' }}>
              <AnalysisReportComponent 
                analysisResult={this.state.analysisResult}
                extraActionArea={this.state.extraActionArea}
                status={this.state.status}
              />
            </div>
            
            {/* Operation Area */}
            <div style={{ 
              display: 'flex', 
              justifyContent: 'center', 
              gap: '16px',
              paddingTop: '16px',
              borderTop: '1px solid #f0f0f0'
            }}>
              <Button 
                type="primary"
                size="large"
                onClick={this.restartAnalysis}
                style={{
                  minWidth: '120px',
                  height: '40px',
                  fontSize: '14px'
                }}
              >
                Analyze Again
              </Button>
            </div>
            <SettingsButton onValidationComplete={this.performValidation} />
          </div>
        );

      default:
        return null;
    }
  };

  render() {
    return (
      <div style={{
        backgroundColor: '#F2F2F3',
        width: '100%',
        height: '100%',
      }}>
        {this.renderContent()}
      </div>
    );
  }
}
