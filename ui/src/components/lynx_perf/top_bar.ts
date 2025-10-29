// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

import m from 'mithril';
import {RightSidebarTab} from '../../lynx_perf/types';
import {lynxPerfGlobals} from '../../lynx_perf/lynx_perf_globals';
import {eventLoggerState} from '../../event_logger';
import {PopupMenu, MenuItem} from '../../widgets/menu';
import {Button} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {PopupPosition} from '../../widgets/popup';
import {llmState} from '../../lynx_perf/llm_state';

export function getScreenSize(): 'large' | 'medium' | 'small' {
  const width =
    window.innerWidth -
    parseInt(
      getComputedStyle(document.documentElement).getPropertyValue(
        '--right-sidebar-width',
      ),
    );
  if (width >= 1400) return 'large';
  if (width >= 800) return 'medium';
  return 'small';
}

export function renderLynxButtons() {
  const screenSize = getScreenSize();

  if (lynxPerfGlobals.state.lynxviewInstances.length === 0) {
    return null;
  }

  const assistantAction = () => {
    if (
      lynxPerfGlobals.state.rightSidebarTab === RightSidebarTab.TraceAssistant
    ) {
      lynxPerfGlobals.closeRightSidebar();
    } else {
      lynxPerfGlobals.changeRightSidebarTab(RightSidebarTab.TraceAssistant);
      eventLoggerState.state.eventLogger.logEvent(
        'ai_analysis_entry_click',
        {},
      );
    }
  };

  const lynxViewAction = () => {
    if (lynxPerfGlobals.state.rightSidebarTab === RightSidebarTab.LynxView) {
      lynxPerfGlobals.closeRightSidebar();
    } else {
      lynxPerfGlobals.changeRightSidebarTab(RightSidebarTab.LynxView);
    }
  };

  if (screenSize === 'small') {
    // Show only overflow menu for small screens
    return m(
      PopupMenu,
      {
        trigger: m(Button, {
          className: 'lynx-overflow-menu',
          icon: 'more_horiz',
          intent: Intent.Primary,
        }),
        popupPosition: PopupPosition.BottomEnd,
      },
      llmState.state.showAnalysisEntry &&
        m(MenuItem, {
          label: 'Trace Analysis',
          icon: 'mindfulness',
          onclick: assistantAction,
        }),
      m(MenuItem, {
        label: 'Focus LynxView',
        icon: 'center_focus_strong',
        onclick: lynxViewAction,
      }),
    );
  } else if (screenSize === 'medium') {
    // Show icons only for medium screens
    return [
      llmState.state.showAnalysisEntry &&
        m(Button, {
          className: 'lynx-assistant',
          icon: 'mindfulness',
          intent: Intent.Primary,
          onclick: assistantAction,
        }),
      m(Button, {
        className: 'lynx-menu',
        icon: 'center_focus_strong',
        intent: Intent.Primary,
        onclick: lynxViewAction,
      }),
    ];
  } else {
    // Show full buttons with labels for large screens
    return [
      llmState.state.showAnalysisEntry &&
        m(Button, {
          className: 'lynx-assistant',
          label: 'Trace Analysis',
          icon: 'mindfulness',
          intent: Intent.Primary,
          onclick: assistantAction,
        }),
      m(Button, {
        className: 'lynx-menu',
        label: 'Focus LynxView',
        icon: 'center_focus_strong',
        intent: Intent.Primary,
        onclick: lynxViewAction,
      }),
    ];
  }
}
