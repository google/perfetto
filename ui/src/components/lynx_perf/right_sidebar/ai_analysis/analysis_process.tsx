// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import React from 'react';
import { ChevronDown, ChevronRight, CheckCircle, Circle, Loader } from 'lucide-react';
import { AnalysisStep } from '../../../../lynx_perf/llm_state';



interface AnalysisProcessProps {
  steps: AnalysisStep[];
}

interface AnalysisProcessState {
  expandedSteps: Set<string>;
}

export class AnalysisProcess extends React.Component<AnalysisProcessProps, AnalysisProcessState> {
  constructor(props: AnalysisProcessProps) {
    super(props);
    this.state = {
      expandedSteps: new Set()
    };
  }

  toggleStep = (stepId: string) => {
    const { expandedSteps } = this.state;
    const newExpanded = new Set(expandedSteps);
    
    if (newExpanded.has(stepId)) {
      newExpanded.delete(stepId);
    } else {
      newExpanded.add(stepId);
    }
    
    this.setState({ expandedSteps: newExpanded });
  };

  getStatusIcon = (status: string) => {
    switch (status) {
      case 'finish':
        return <CheckCircle size={16} style={{ color: '#22c55e' }} />;
      case 'process':
        return <Loader size={16} style={{ 
          color: '#3b82f6',
          animation: 'ai-analysis-spin 1s linear infinite'
        }} />;
      case 'error':
        return <Circle size={16} style={{ color: '#ef4444' }} />;
      default:
        return <Circle size={16} style={{ color: '#d1d5db' }} />;
    }
  };

  render() {
    const { steps } = this.props;
    const { expandedSteps } = this.state;

    return (
      <div>
        <h3 style={{
          fontSize: '1.125rem',
          fontWeight: 600,
          color: '#111827',
          marginBottom: '16px'
        }}>Analysis Process</h3>
        
        <div style={{
          display: 'flex',
          flexDirection: 'column',
          gap: '12px'
        }}>
          {steps.map((step: AnalysisStep) => (
            <div key={step.id} style={{
              backgroundColor: '#ffffff',
              border: '1px solid #e5e7eb',
              borderRadius: '12px',
              boxShadow: '0 1px 2px 0 rgba(0, 0, 0, 0.05)',
              transition: 'all 0.3s ease-in-out'
            }}>
              {/* Step Header */}
              <div 
                style={{
                  display: 'flex',
                  alignItems: 'center',
                  justifyContent: 'space-between',
                  padding: '16px',
                  cursor: step.details.length > 0 ? 'pointer' : 'default',
                  borderBottom: expandedSteps.has(step.id) && step.details.length > 0 ? '1px solid #e5e7eb' : 'none'
                }}
                onClick={() => step.details.length > 0 && this.toggleStep(step.id)}
              >
                <div style={{
                  display: 'flex',
                  alignItems: 'center',
                  gap: '12px'
                }}>
                  <div style={{
                    flexShrink: 0
                  }}>
                    {this.getStatusIcon(step.status)}
                  </div>
                  <span style={{
                    fontWeight: 500,
                    color: '#121212',
                    wordBreak: 'break-word',
                    flex: 1
                  }}>
                    {step.title}
                  </span>
                </div>
                {step.details.length > 0 && (
                  expandedSteps.has(step.id) ? 
                    <ChevronDown size={16} style={{ color: '#121212', flexShrink: 0 }} /> : 
                    <ChevronRight size={16} style={{ color: '#121212', flexShrink: 0 }} />
                )}
              </div>

              {/* Step Details */}
              {expandedSteps.has(step.id) && step.details.length > 0 && (
                <div style={{ padding: '16px' }}>
                  <div style={{
                    display: 'flex',
                    flexDirection: 'column',
                    gap: '8px'
                  }}>
                    {step.details.map((detail: string, idx: number) => (
                      <div key={idx} style={{
                        fontSize: '0.875rem',
                        color: '#6b7280',
                        lineHeight: 1.5,
                        paddingLeft: '8px',
                        borderLeft: '2px solid #d1d5db',
                        wordBreak: 'break-word',
                        overflowWrap: 'break-word',
                        maxWidth: '100%'
                      }}>
                        • {detail}
                      </div>
                    ))}
                  </div>
                </div>
              )}
            </div>
          ))}
        </div>
      </div>
    );
  }
}