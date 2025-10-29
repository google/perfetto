// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import { Component } from 'react';
import ReactMarkdown from 'react-markdown';
import { Card } from 'antd';
import { Router } from '../../../../core/router';
import { AppImpl } from '../../../../core/app_impl';

interface AnalysisReportProps {
  analysisResult: string;
  extraActionArea?: React.ReactNode;
  status?: string;
}

export class AnalysisReportComponent extends Component<AnalysisReportProps> {
  private markdownClick: (e: MouseEvent) => void;
  private isEventListenerAdded = false;

  constructor(props: AnalysisReportProps) {
    super(props);
    this.markdownClick = this.handleMarkdownClick.bind(this);
  }

  private handleMarkdownClick = (e: MouseEvent) => {
    if (e.target && e.target instanceof HTMLElement && e.target.tagName === 'A') {
      const href = e.target.getAttribute('href');

      if (href) {
        const sliceId = this.getSliceIdFromUrl(href);
        if (sliceId) {
          e.preventDefault();
          AppImpl.instance.trace?.selection.selectSqlEvent('slice', parseInt(sliceId), {
            scrollToSelection: true,
          });
        }
      }
    }
  };

  private getSliceIdFromUrl = (href: string) => {
    try {
      const router = Router.parseUrl(href);
      return router.args.sliceId ?? null;
    } catch (error) {
      return null;
    }
  };

  componentDidUpdate(prevProps: AnalysisReportProps, _prevState: {}) {
    if (
      prevProps.status === 'completed' &&
      !this.isEventListenerAdded
    ) {
      addEventListener('click', this.markdownClick);
      this.isEventListenerAdded = true;
      console.log('Markdown click event listener added');
    }

    // Remove event listener when status changes from completed to other states
    if (
      prevProps.status !== 'completed' &&
      this.isEventListenerAdded
    ) {
      this.removeMarkdownClickListener();
    }
  }

  private removeMarkdownClickListener() {
    if (this.isEventListenerAdded) {
      removeEventListener('click', this.markdownClick);
      this.isEventListenerAdded = false;
      console.log('Markdown click event listener removed');
    }
  }

  componentWillUnmount() {
    this.removeMarkdownClickListener();
  }

  render() {
    const { analysisResult, extraActionArea } = this.props;

    return (
      <div style={{ marginBottom: '24px', flex: 1 }}>
        <div style={{ 
          display: 'flex', 
          justifyContent: 'space-between', 
          alignItems: 'center', 
          marginBottom: '16px' 
        }}>
          <h3 style={{ fontSize: '18px', fontWeight: '600', color: '#262626', margin: 0 }}>
            Analysis Report
          </h3>
          {extraActionArea}
        </div>
        
        <Card
          bodyStyle={{
            padding: '16px'
          }}
        >
          <div
            style={{
              wordBreak: 'break-word',
              overflowWrap: 'break-word',
              maxWidth: '100%'
            }}
          >
            <ReactMarkdown
              components={{
                h2: ({ children }: any) => (
                  <h2 style={{ color: '#000000', fontSize: '18px', fontWeight: '700', marginBottom: '12px' }}>
                    {children}
                  </h2>
                ),
                h3: ({ children }: any) => (
                  <h3 style={{ color: '#000000', fontSize: '16px', fontWeight: '600', marginBottom: '8px' }}>
                    {children}
                  </h3>
                ),
                a: ({ href, children }: any) => (
                  <a
                    href={href}
                    style={{ color: '#1890ff', textDecoration: 'underline' }}
                  >
                    {children}
                  </a>
                ),
              }}
            >
              {analysisResult}
            </ReactMarkdown>
          </div>
        </Card>
      </div>
    );
  }
}

export default AnalysisReportComponent;