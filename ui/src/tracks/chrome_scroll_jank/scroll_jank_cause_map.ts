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

import {exists} from '../../base/utils';
import {Engine} from '../../trace_processor/engine';
import {STR} from '../../trace_processor/query_result';

export enum CauseProcess {
  UNKNOWN,
  BROWSER = 'Browser',
  RENDERER = 'Renderer',
  GPU = 'GPU',
}

export enum CauseThread {
  UNKNOWN,
  BROWSER_MAIN = 'CrBrowserMain',
  RENDERER_MAIN = 'CrRendererMain',
  COMPOSITOR = 'Compositor',
  CHROME_CHILD_IO_THREAD = 'Chrome_ChildIOThread',
  VIZ_COMPOSITOR = 'VizCompositorThread',
  SURFACE_FLINGER = 'surfaceflinger',
}

export interface ScrollJankCause {
  description: string;
  process: CauseProcess;
  thread: CauseThread;
}

export interface EventLatencyStageDetails {
  description: string;
  jankCauses: ScrollJankCause[];
}

export interface ScrollJankCauseMapInternal {
  // Key corresponds with the EventLatency stage.
  [key: string]: EventLatencyStageDetails;
}

function getScrollJankProcess(process: string): CauseProcess {
  switch (process) {
    case CauseProcess.BROWSER:
      return CauseProcess.BROWSER;
    case CauseProcess.RENDERER:
      return CauseProcess.RENDERER;
    case CauseProcess.GPU:
      return CauseProcess.GPU;
    default:
      return CauseProcess.UNKNOWN;
  }
}

function getScrollJankThread(thread: string): CauseThread {
  switch (thread) {
    case CauseThread.BROWSER_MAIN:
      return CauseThread.BROWSER_MAIN;
    case CauseThread.RENDERER_MAIN:
      return CauseThread.RENDERER_MAIN;
    case CauseThread.CHROME_CHILD_IO_THREAD:
      return CauseThread.CHROME_CHILD_IO_THREAD;
    case CauseThread.COMPOSITOR:
      return CauseThread.COMPOSITOR;
    case CauseThread.VIZ_COMPOSITOR:
      return CauseThread.VIZ_COMPOSITOR;
    case CauseThread.SURFACE_FLINGER:
      return CauseThread.SURFACE_FLINGER;
    default:
      return CauseThread.UNKNOWN;
  }
}

export class ScrollJankCauseMap {
  private static instance: ScrollJankCauseMap;
  private causes: ScrollJankCauseMapInternal;

  private constructor() {
    this.causes = {};
  }

  private async initializeCauseMap(engine: Engine) {
    const queryResult = await engine.query(`
      INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_cause_map;

      SELECT
        IFNULL(name, '') AS name,
        IFNULL(description, '') AS description,
        IFNULL(cause_process, '') AS causeProcess,
        IFNULL(cause_thread, '') AS causeThread,
        IFNULL(cause_description, '') AS causeDescription
      FROM chrome_scroll_jank_causes_with_event_latencies;
    `);

    const iter = queryResult.iter({
      name: STR,
      description: STR,
      causeProcess: STR,
      causeThread: STR,
      causeDescription: STR,
    });

    for (; iter.valid(); iter.next()) {
      const eventLatencyStage = iter.name;
      if (!(eventLatencyStage in this.causes)) {
        this.causes[eventLatencyStage] = {
          description: iter.description,
          jankCauses: [] as ScrollJankCause[],
        };
      }

      const causeProcess = getScrollJankProcess(iter.causeProcess);
      const causeThread = getScrollJankThread(iter.causeThread);

      this.causes[eventLatencyStage].jankCauses.push({
        description: iter.causeDescription,
        process: causeProcess,
        thread: causeThread,
      });
    }
  }

  // Must be called before this item is accessed, as the object is populated
  // from SQL data.
  public static async initialize(engine: Engine) {
    if (!exists(ScrollJankCauseMap.instance)) {
      ScrollJankCauseMap.instance = new ScrollJankCauseMap();
      await ScrollJankCauseMap.instance.initializeCauseMap(engine);
    }
  }

  public static getEventLatencyDetails(
    eventLatency: string,
  ): EventLatencyStageDetails | undefined {
    if (eventLatency in ScrollJankCauseMap.instance.causes) {
      return ScrollJankCauseMap.instance.causes[eventLatency];
    }
    return undefined;
  }
}
