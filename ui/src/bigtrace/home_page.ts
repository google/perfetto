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

import m from 'mithril';
import {RecentQueriesSection} from './recent_queries';
import {Icon} from '../widgets/icon';
import {queryState} from './query_state';

interface HomePageAttrs {
  navigateTo: (page: string) => void;
}

const SLICE_COUNT_QUERY = `SELECT
  COUNT(*) as slice_count
FROM slice`;

const CPU_TIME_QUERY = `SELECT
  p.name,
  sum(s.dur)/1e9 as cpu_sec
FROM sched s
JOIN thread t USING (utid)
JOIN process p USING (upid)
GROUP BY p.name
ORDER BY cpu_sec DESC
LIMIT 10`;

const homePageStyles = `
.pf-home-page {
  padding: 3rem;
  overflow-y: auto;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 3rem;
  background-color: var(--pf-bg-primary, #fff);
  color: var(--pf-fg-primary, #202124);
}

.pf-theme-provider--dark .pf-home-page {
  background-color: #121212;
  color: #e8eaed;
}

.pf-theme-provider--light .pf-home-page {
  background-color: #f8f9fa;
  color: #202124;
}

.pf-home-header {
  text-align: center;
  max-width: 800px;
}

.pf-home-title {
  font-size: 3.5rem;
  font-weight: 800;
  margin-bottom: 1rem;
  background: linear-gradient(135deg, #1A73E8 0%, #E91E63 100%);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  letter-spacing: -0.02em;
}

.pf-home-subtitle {
  font-size: 1.25rem;
  line-height: 1.6;
  max-width: 600px;
  margin: 0 auto;
  color: var(--pf-fg-secondary, #5f6368);
}

.pf-theme-provider--dark .pf-home-subtitle {
  color: #9aa0a6;
}

.pf-home-section {
  width: 100%;
  max-width: 1000px;
}

.pf-home-section-title {
  font-size: 1.5rem;
  font-weight: 600;
  margin-bottom: 2rem;
  padding-bottom: 0.75rem;
  display: flex;
  align-items: center;
  gap: 0.5rem;
  color: var(--pf-fg-primary, #202124);
  border-bottom: 1px solid var(--pf-border-color, #dadce0);
}

.pf-theme-provider--dark .pf-home-section-title {
  color: #e8eaed;
  border-bottom-color: #3c4043;
}

.pf-home-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
  gap: 2rem;
  width: 100%;
}

.pf-home-card {
  border-radius: 12px;
  padding: 2.5rem;
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 1.25rem;
  transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
  height: 100%;
  background: var(--pf-bg-secondary, #ffffff);
  border: 1px solid var(--pf-border-color, #dadce0);
  box-shadow: 0 2px 4px rgba(0, 0, 0, 0.05);
}

.pf-theme-provider--dark .pf-home-card {
  background: #202124;
  border-color: #3c4043;
  box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
}

.pf-home-card.pf-interactive {
  cursor: pointer;
}

.pf-home-card.pf-interactive:hover {
  transform: translateY(-8px);
  box-shadow: 0 12px 24px rgba(0, 0, 0, 0.1);
  border-color: #1A73E8;
}

.pf-theme-provider--dark .pf-home-card.pf-interactive:hover {
  box-shadow: 0 12px 24px rgba(0, 0, 0, 0.3);
  border-color: #8ab4f8;
}

.pf-home-icon-box {
  display: flex;
  justify-content: center;
  align-items: center;
  width: 56px;
  height: 56px;
  border-radius: 16px;
  background: rgba(26, 115, 232, 0.1);
  color: #1A73E8;
  margin-bottom: 0.5rem;
  transition: all 0.3s;
}

.pf-theme-provider--dark .pf-home-icon-box {
  background: rgba(138, 180, 248, 0.1);
  color: #8ab4f8;
}

.pf-home-card:hover .pf-home-icon-box {
  background: #1A73E8;
  color: white;
  transform: scale(1.1) rotate(5deg);
}

.pf-theme-provider--dark .pf-home-card:hover .pf-home-icon-box {
  background: #8ab4f8;
  color: #202124;
}

.pf-home-card-title {
  font-size: 1.25rem;
  font-weight: 600;
  margin: 0;
  color: var(--pf-fg-primary, #202124);
}

.pf-theme-provider--dark .pf-home-card-title {
  color: #e8eaed;
}

.pf-home-card-description {
  font-size: 1rem;
  line-height: 1.5;
  margin: 0;
  color: var(--pf-fg-secondary, #5f6368);
}

.pf-theme-provider--dark .pf-home-card-description {
  color: #9aa0a6;
}

.pf-home-code-card {
  border-radius: 8px !important;
  width: 100%;
  position: relative;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  gap: 1rem;
  background: #f1f3f4 !important;
  border: 1px solid #dadce0 !important;
  padding: 1.5rem !important;
}

.pf-theme-provider--dark .pf-home-code-card {
  background: #1e1e1e !important;
  border-color: #3c4043 !important;
}

.pf-home-code-card::before {
  content: '';
  position: absolute;
  top: 0;
  left: 0;
  right: 0;
  height: 4px;
  background: #1A73E8;
}

.pf-theme-provider--dark .pf-home-code-card::before {
  background: #8ab4f8;
}

.pf-home-code-content {
  margin: 0;
  font-family: 'Roboto Mono', monospace;
  font-size: 0.95rem;
  line-height: 1.5;
  max-height: 200px;
  overflow-y: auto;
  white-space: pre-wrap;
  color: #3c4043;
}

.pf-theme-provider--dark .pf-home-code-content {
  color: #e8eaed;
}

/* Scrollbar styling */
.pf-home-code-content::-webkit-scrollbar {
  width: 8px;
  height: 8px;
}

.pf-home-code-content::-webkit-scrollbar-track {
  background: #f1f3f4;
}

.pf-theme-provider--dark .pf-home-code-content::-webkit-scrollbar-track {
  background: #111;
}

.pf-home-code-content::-webkit-scrollbar-thumb {
  background: #dadce0;
  border-radius: 4px;
}

.pf-theme-provider--dark .pf-home-code-content::-webkit-scrollbar-thumb {
  background: #444;
}

.pf-home-code-content::-webkit-scrollbar-thumb:hover {
  background: #bdc1c6;
}

.pf-theme-provider--dark .pf-home-code-content::-webkit-scrollbar-thumb:hover {
  background: #555;
}
`;

if (typeof document !== 'undefined') {
  const styleEl = document.createElement('style');
  styleEl.textContent = homePageStyles;
  document.head.appendChild(styleEl);
}

export class HomePage implements m.ClassComponent<HomePageAttrs> {
  view({attrs}: m.Vnode<HomePageAttrs>) {
    return m(
        '.pf-home-page',
        m('.pf-home-header',
          m('h1.pf-home-title', 'Welcome to BigTrace'),
          m('p.pf-home-subtitle', 'Analyze traces at scale 🚀. BigTrace helps you find bugs 🐛 and performance issues 🐢 across thousands of traces.')
        ),
        
        m('.pf-home-section',
          m('.pf-home-section-title', m(Icon, {icon: 'play_arrow'}), 'How to get started'),
          m('.pf-home-grid',
            m('.pf-home-card',
              m('.pf-home-icon-box', m(Icon, {icon: 'edit', filled: true})),
              m('h3.pf-home-card-title', '1. Write a query'),
              m('p.pf-home-card-description', 'Use the query editor to write a PerfettoSQL query.')
            ),
            m('.pf-home-card',
              m('.pf-home-icon-box', m(Icon, {icon: 'play_arrow', filled: true})),
              m('h3.pf-home-card-title', '2. Run it'),
              m('p.pf-home-card-description', 'Click "Run Query" or press Cmd/Ctrl + Enter.')
            ),
            m('.pf-home-card',
              m('.pf-home-icon-box', m(Icon, {icon: 'analytics', filled: true})),
              m('h3.pf-home-card-title', '3. Analyze'),
              m('p.pf-home-card-description', 'Browse the results in the table below.')
            )
          )
        ),
        
        m('.pf-home-section',
          m('.pf-home-section-title', m(Icon, {icon: 'description'}), 'Examples'),
          m('.pf-home-grid',
            m('.pf-home-card.pf-interactive', {
              onclick: () => {
                queryState.initialQuery = SLICE_COUNT_QUERY;
                attrs.navigateTo('bigtrace');
              },
            }, 
              m('.pf-home-icon-box', m(Icon, {icon: 'search'})),
              m('h3.pf-home-card-title', 'Slice Count'),
              m('p.pf-home-card-description', 'Count the total number of slices in the trace.'),
              m('.pf-home-code-card', m('pre.pf-home-code-content', SLICE_COUNT_QUERY))
            ),
            m('.pf-home-card.pf-interactive', {
              onclick: () => {
                queryState.initialQuery = CPU_TIME_QUERY;
                attrs.navigateTo('bigtrace');
              },
            },
              m('.pf-home-icon-box', m(Icon, {icon: 'timer'})),
              m('h3.pf-home-card-title', 'Top CPU Consumers'),
              m('p.pf-home-card-description', 'Find the processes using the most CPU time.'),
              m('.pf-home-code-card', m('pre.pf-home-code-content', CPU_TIME_QUERY))
            )
          )
        ),
        m(RecentQueriesSection, {
          onLoadQuery: (query: string) => {
            queryState.initialQuery = query;
            attrs.navigateTo('bigtrace');
          },
        })
    );
  }
}
