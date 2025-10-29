// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import {createStore} from '../base/store';

export interface LLMConfig {
  modelProvider: string;
  modelName: string;
  apiKey: string;
  baseUrl?: string;
  customPrompt?: string;
}

export interface AnalysisStep {
  id: string;
  title: string;
  status: 'wait' | 'process' | 'finish' | 'error';
  details: string[];
  collapsed?: boolean;
}

export interface StepListener {
  onStepUpdate(
    stepId: string,
    title: string,
    status: 'wait' | 'process' | 'finish' | 'error',
    content: string,
  ): void;
}

export interface AnalysisReport {
  analysisResult: string;
  extraActionProperties: Record<string, string>;
  analysisSteps: AnalysisStep[];
  extraActionArea?: React.ReactNode;
}

export interface ReportExtraAction {
  render(
    // TODO: fix the type of results to be TraceAnalysisResult[]
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    results: any[] | undefined,
    steps: AnalysisStep[] | undefined,
    actionProperties: Record<string, string> | undefined,
  ): Promise<React.ReactNode | undefined>;

  getActionProperties(): Record<string, string> | undefined;

  getHistoryAnalysisReport(): Promise<AnalysisReport | undefined>;

  saveAnalysisReport(result: AnalysisReport): Promise<boolean>;

  // TODO: fix the type of results to be TraceAnalysisResult[]
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  generateCharts(traceResult: any): Promise<string[]>;
}

export interface TraceAnalysis {
  analysis(stepListener: StepListener): Promise<AnalysisReport | undefined>;
}

interface State {
  showAnalysisEntry: boolean;
  config: LLMConfig;
  reportExtraAction: ReportExtraAction | undefined;
  traceAnalysis: TraceAnalysis | undefined;
}

const emptyState: State = {
  showAnalysisEntry: false,
  config: {
    modelProvider: '',
    modelName: '',
    apiKey: '',
    baseUrl: '',
    customPrompt: '',
  },
  reportExtraAction: undefined,
  traceAnalysis: undefined,
};

export const llmState = createStore<State>(emptyState);

export function updateLLMConfig(config: LLMConfig) {
  llmState.edit((draft) => {
    Object.assign(draft.config, config);
  });
}

export function updateReportExtraAction(
  extraAction: ReportExtraAction | undefined,
) {
  llmState.edit((draft) => {
    draft.reportExtraAction = extraAction;
  });
}
