// Copyright (C) 2023 The Android Open Source Project
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
import {AppImpl} from '../../core/app_impl';
import {Anchor} from '../../widgets/anchor';
import {Button, ButtonVariant} from '../../widgets/button';
import {CardStack} from '../../widgets/card';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {closeModal, redrawModal, showModal} from '../../widgets/modal';
import {Callout} from '../../widgets/callout';
import {Spinner} from '../../widgets/spinner';
import {Stack} from '../../widgets/stack';
import {TabStrip, TabOption} from '../../widgets/tabs';
import {TextParagraph} from '../../widgets/text_paragraph';
import {MultiTraceController} from './multi_trace_controller';
import {TraceFile} from './multi_trace_types';
import {WasmTraceAnalyzer} from './trace_analyzer';

const MODAL_KEY = 'multi-trace-modal';

// =============================================================================
// Shell Component
// =============================================================================

interface MultiTraceModalAttrs {
  initialFiles: ReadonlyArray<File>;
}

class MultiTraceModalShell implements m.ClassComponent<MultiTraceModalAttrs> {
  private controller = new MultiTraceController(new WasmTraceAnalyzer(), () =>
    redrawModal(),
  );
  private currentTab = 'synchronous';

  oncreate({attrs}: m.Vnode<MultiTraceModalAttrs>) {
    this.controller.addFiles(attrs.initialFiles);
  }

  view() {
    return m(
      Stack,
      {className: 'pf-multi-trace-modal', orientation: 'vertical'},
      this.renderDescription(),
      m(TraceListComponent, {
        traces: this.controller.traces,
        controller: this.controller,
      }),
      m(
        Stack,
        {className: 'pf-multi-trace-modal__footer', orientation: 'horizontal'},
        this.renderActions(),
      ),
    );
  }

  private renderDescription() {
    const tabs: TabOption[] = [
      {key: 'synchronous', title: 'Synchronous Traces'},
      {key: 'cross-machine', title: 'Cross-Machine Traces'},
      {key: 'comparison', title: 'Trace Comparison'},
    ];

    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__description-panel',
        orientation: 'vertical',
      },
      m(TabStrip, {
        className: 'pf-multi-trace-modal__tabs',
        tabs,
        currentTabKey: this.currentTab,
        onTabChange: (key: string) => {
          this.currentTab = key;
          redrawModal();
        },
      }),
      m('.pf-multi-trace-modal__description-content', this.renderTabContent()),
    );
  }

  private renderTabContent() {
    switch (this.currentTab) {
      case 'synchronous':
        return [
          m(TextParagraph, {
            text: '🔗 Combine multiple trace files that were captured at the same time on the same device or system. This allows you to view traces from different sources (e.g., system traces, app traces, custom instrumentation) on a unified timeline.',
          }),
        ];
      case 'cross-machine':
        return [
          m(TextParagraph, {
            text: '🌐 Merge traces captured on different machines or devices with distributed time synchronization.',
          }),
        ];
      case 'comparison':
        return [
          m(TextParagraph, {
            text: '📊 Compare traces from different time periods to identify performance regressions or improvements.',
          }),
        ];
      default:
        return '';
    }
  }

  private renderActions() {
    const footerContent = this.getFooterContent();
    const isDisabled = footerContent !== undefined;
    const openButton = m(Button, {
      label: 'Open Traces',
      intent: Intent.Primary,
      variant: ButtonVariant.Filled,
      onclick: () => this.openTraces(),
      disabled: isDisabled,
    });

    if (footerContent !== undefined) {
      return [
        m(
          Callout,
          {
            className: 'pf-multi-trace-modal__footer-error',
            intent: Intent.Danger,
            icon: 'error_outline',
          },
          footerContent,
        ),
        m('.pf-multi-trace-modal__footer-spacer'),
        openButton,
      ];
    } else {
      return [m('.pf-multi-trace-modal__footer-spacer'), openButton];
    }
  }

  private getFooterContent(): m.Children | undefined {
    if (this.currentTab === 'cross-machine') {
      return [
        'This feature is not yet supported. Please +1 ',
        m(
          Anchor,
          {
            href: 'https://github.com/google/perfetto/issues/2781',
            target: '_blank',
          },
          'this GitHub issue',
        ),
        ' to prioritize development, or select "Synchronous Traces" to continue.',
      ];
    }
    if (this.currentTab === 'comparison') {
      return [
        'This feature is not yet supported. Please +1 ',
        m(
          Anchor,
          {
            href: 'https://github.com/google/perfetto/issues/2780',
            target: '_blank',
          },
          'this GitHub issue',
        ),
        ' to prioritize development, or select "Synchronous Traces" to continue.',
      ];
    }

    const error = this.controller.getLoadingError();
    if (error === undefined) {
      return undefined;
    }
    switch (error) {
      case 'NO_TRACES':
        return 'Add at least one trace to open.';
      case 'ANALYZING':
        return 'Wait for all traces to be analyzed.';
      case 'TRACE_ERROR':
        return 'Remove traces with errors before opening.';
      default:
        return 'An unknown error occurred.';
    }
  }

  private openTraces() {
    if (this.controller.traces.length === 0) {
      return;
    }
    const files = this.controller.traces.map((t) => t.file);
    AppImpl.instance.openTraceFromMultipleFiles(files);
    closeModal(MODAL_KEY);
  }
}

// =============================================================================
// Trace List Component
// =============================================================================

interface TraceListComponentAttrs {
  traces: ReadonlyArray<TraceFile>;
  controller: MultiTraceController;
}

class TraceListComponent implements m.ClassComponent<TraceListComponentAttrs> {
  view({attrs}: m.Vnode<TraceListComponentAttrs>) {
    const {traces, controller} = attrs;
    return m(
      Stack,
      {className: 'pf-multi-trace-modal__list-panel', orientation: 'vertical'},
      traces.map((trace) => this.renderTraceItem(trace, controller)),
      m(
        CardStack,
        {
          className: 'pf-multi-trace-modal__add-card',
          onclick: () => this.addTraces(controller),
        },
        m(Icon, {icon: 'add'}),
        'Add more traces',
      ),
    );
  }

  private renderTraceItem(trace: TraceFile, controller: MultiTraceController) {
    return m(
      CardStack,
      {
        className: 'pf-multi-trace-modal__card',
        direction: 'horizontal',
        key: trace.uuid,
      },
      this.renderTraceInfo(trace),
      this.renderCardActions(trace, controller),
    );
  }

  private renderTraceInfo(trace: TraceFile) {
    return m(
      Stack,
      {
        className: 'pf-multi-trace-modal__info',
        spacing: 'large',
        orientation: 'vertical',
      },
      m('.pf-multi-trace-modal__name', trace.file.name),
      m(
        Stack,
        {orientation: 'horizontal', spacing: 'large'},
        m(
          Stack,
          {
            className: 'pf-multi-trace-modal__size',
            orientation: 'horizontal',
          },
          m('strong', 'Size:'),
          m('span', `${(trace.file.size / (1024 * 1024)).toFixed(1)} MB`),
        ),
        trace.status === 'analyzed'
          ? m(
              Stack,
              {
                className: 'pf-multi-trace-modal__format',
                orientation: 'horizontal',
              },
              m('strong', 'Format:'),
              m('span', trace.format),
            )
          : this.renderTraceStatus(trace),
      ),
    );
  }

  private renderCardActions(
    trace: TraceFile,
    controller: MultiTraceController,
  ) {
    return m(
      '.pf-multi-trace-modal__actions',
      m(Button, {
        icon: 'delete',
        onclick: () => controller.removeTrace(trace.uuid),
        disabled: controller.isAnalyzing(),
      }),
    );
  }

  private renderTraceStatus(trace: TraceFile) {
    const statusInfo = getStatusInfo(trace);
    const progressText =
      trace.status === 'analyzing'
        ? ` (${(trace.progress * 100).toFixed(0)}%)`
        : '';
    return m(
      Stack,
      {
        orientation: 'horizontal',
        className: 'pf-multi-trace-modal__status-wrapper',
        spacing: 'small',
      },
      trace.status === 'analyzing' && m(Spinner),
      m(
        '.pf-multi-trace-modal__status' + statusInfo.class,
        `${statusInfo.text}${progressText}`,
      ),
    );
  }

  private addTraces(controller: MultiTraceController) {
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.addEventListener('change', () => {
      if (input.files) {
        controller.addFiles([...input.files]);
      }
    });
    input.click();
  }
}

// =============================================================================
// Public API & Helpers
// =============================================================================

export function showMultiTraceModal(initialFiles: ReadonlyArray<File>) {
  showModal({
    title: 'Open Multiple Traces',
    icon: 'library_books',
    key: MODAL_KEY,
    className: 'pf-multi-trace-modal-override',
    content: () => m(MultiTraceModalShell, {initialFiles}),
  });
}

function getStatusInfo(trace: TraceFile) {
  switch (trace.status) {
    case 'analyzed':
      return {
        class: '.pf-multi-trace-modal__status--analyzed',
        text: 'Analyzed',
      };
    case 'analyzing':
      return {
        class: '.pf-multi-trace-modal__status--analyzing',
        text: 'Analyzing...',
      };
    case 'not-analyzed':
      return {
        class: '',
        text: 'Not analyzed',
      };
    case 'error':
      return {
        class: '.pf-multi-trace-modal__status--error',
        text: 'Error',
      };
    default:
      return {
        class: '',
        text: 'Unknown',
      };
  }
}
