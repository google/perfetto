// Copyright (C) 2026 The Android Open Source Project
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

// Analysis provider registry for the GPU Compute plugin.
//
// Defines the interfaces and registry functions that allow an analysis
// provider plugin to inject analysis UI into the parent plugin. When no
// analysis provider is registered, the parent plugin gracefully omits
// analysis features.
//
// This follows the same pattern as {@link ./section} and
// {@link ./terminology}.

import m from 'mithril';
import type {Engine} from '../../trace_processor/engine';
import type {MetricSection, KernelMetricData} from './details';

// Result of a performance analysis.
export interface PerformanceAnalysisResult {
  // The section that was analyzed
  sectionName: string;
  // The generated analysis
  analysis: string;
  // Whether the analysis completed successfully
  success: boolean;
  // Error message if analysis failed
  error?: string;
  // The name of the provider used to generate the analysis
  providerName?: string;
}

// Interface for analysis cache operations passed from parent component.
export interface AnalysisCache {
  getKernelAnalysis(sliceId: number): PerformanceAnalysisResult | undefined;
  setKernelAnalysis(sliceId: number, result: PerformanceAnalysisResult): void;
  getSectionAnalysis(
    sliceId: number,
    sectionName: string,
  ): PerformanceAnalysisResult | undefined;
  setSectionAnalysis(
    sliceId: number,
    sectionName: string,
    result: PerformanceAnalysisResult,
  ): void;
}

// Interface implemented by an analysis provider plugin to provide
// analysis UI to the parent GPU Compute plugin.
export interface AnalysisProvider {
  // Renders the full Analysis tab body.
  renderAnalysisTab(attrs: {
    engine: Engine;
    sliceId: number;
    analysisCache: AnalysisCache;
  }): m.Children;

  // Renders a per-section inline analysis button.
  renderSectionAnalysis(attrs: {
    section: MetricSection;
    kernelData: KernelMetricData;
    sliceId: number;
    analysisCache: AnalysisCache;
  }): m.Children;
}

// Singleton registry — set by the Analysis sub-plugin during onActivate.
let analysisProvider: AnalysisProvider | undefined;

// Registers an analysis provider (called by the Analysis sub-plugin).
export function registerAnalysisProvider(provider: AnalysisProvider): void {
  analysisProvider = provider;
}

// Returns the registered analysis provider, or undefined.
export function getAnalysisProvider(): AnalysisProvider | undefined {
  return analysisProvider;
}

// Returns whether an analysis provider has been registered.
export function isAnalysisAvailable(): boolean {
  return analysisProvider !== undefined;
}
