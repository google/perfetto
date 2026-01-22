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

import {Dataset, SourceDataset} from '../../trace_processor/dataset';
import {
  EventContext,
  RELATED_EVENT_SCHEMA,
  RelationRule,
} from '../dev.perfetto.RelatedEvents/relation_finding_strategy';

const hexToDec = (hex: string): number | null => {
  if (hex.startsWith('0x')) hex = hex.substring(2);
  const dec = parseInt(hex, 16);
  return isNaN(dec) ? null : dec;
};

const decToHex = (dec: number): string => '0x' + dec.toString(16);

export class InputIdRule implements RelationRule {
  getRelatedEventsAsDataset(ctx: EventContext): Dataset[] {
    const idMatch = ctx.name.match(/id=(0x[0-9a-fA-F]+)/);
    if (!idMatch) return [];

    const datasets: Dataset[] = [];
    const id = idMatch[1];

    if (ctx.name.includes('UnwantedInteractionBlocker::notifyMotion')) {
      datasets.push(
        new SourceDataset({
          src: `
          SELECT id, name, ts, dur, track_id, 'id_match' as relation
          FROM slice
          WHERE name GLOB 'prepareDispatchCycleLocked(*id=${id}*'
        `,
          schema: RELATED_EVENT_SCHEMA,
        }),
      );
    }

    if (ctx.name.startsWith('prepareDispatchCycleLocked')) {
      datasets.push(
        new SourceDataset({
          src: `
          SELECT DISTINCT id, name, ts, dur, track_id, 'id_match' as relation
          FROM slice
          WHERE name GLOB '*UnwantedInteractionBlocker::notifyMotion(*id=${id}*'
        `,
          schema: RELATED_EVENT_SCHEMA,
        }),
      );

      datasets.push(
        new SourceDataset({
          src: `
          SELECT
            s.id, s.name, s.ts, s.dur, s.track_id, 'descendant_send' as relation
          FROM descendant_slice(${ctx.sliceId}) ds
          JOIN slice s ON ds.id = s.id
          WHERE s.name GLOB 'sendMessage(*type=MOTION*'
        `,
          schema: RELATED_EVENT_SCHEMA,
        }),
      );
    }

    return datasets;
  }
}

export class InputSequenceRule implements RelationRule {
  getRelatedEventsAsDataset(ctx: EventContext): Dataset[] {
    const seqMatch = ctx.name.match(/seq=(0x[0-9a-fA-F]+)/);
    if (!seqMatch) return [];

    const datasets: Dataset[] = [];
    const seqHex = seqMatch[1];
    const seqDec = hexToDec(seqHex);

    if (seqDec !== null) {
      datasets.push(
        new SourceDataset({
          src: `
          SELECT id, name, ts, dur, track_id, 'seq_to_cookie' as relation
          FROM slice
          WHERE name GLOB 'InputConsumer processing on *'
            AND extract_arg(arg_set_id, 'cookie') = ${seqDec}
        `,
          schema: RELATED_EVENT_SCHEMA,
        }),
      );
    }

    datasets.push(
      new SourceDataset({
        src: `
        SELECT DISTINCT
            ancestor.id,
            ancestor.name,
            ancestor.ts,
            ancestor.dur,
            ancestor.track_id,
            'parent_prepare' as relation
        FROM ancestor_slice(${ctx.sliceId}) ancestor
        WHERE ancestor.name GLOB 'prepareDispatchCycleLocked(*'
      `,
        schema: RELATED_EVENT_SCHEMA,
      }),
    );

    return datasets;
  }
}

export class InputConsumerRule implements RelationRule {
  getRelatedEventsAsDataset(ctx: EventContext): Dataset[] {
    const cookieVal = ctx.args.get('cookie');
    if (ctx.name.startsWith('InputConsumer processing on') && cookieVal) {
      const cookieDec = parseInt(cookieVal, 10);
      if (!isNaN(cookieDec)) {
        const seqHex = decToHex(cookieDec);

        return [
          new SourceDataset({
            src: `
              SELECT DISTINCT id, name, ts, dur, track_id, 'cookie_to_seq' as relation
              FROM slice
              WHERE name GLOB '*seq=${seqHex}*' 
                AND (name GLOB 'sendMessage(*type=MOTION*' OR name GLOB 'receiveMessage(*type=MOTION*')
            `,
            schema: RELATED_EVENT_SCHEMA,
          }),
        ];
      }
    }
    return [];
  }
}

export class ChoreographerRule implements RelationRule {
  getRelatedEventsAsDataset(ctx: EventContext): Dataset[] {
    const datasets: Dataset[] = [];

    if (ctx.name.startsWith('deliverInputEvent src')) {
      datasets.push(
        new SourceDataset({
          src: `
          SELECT DISTINCT
            ancestor.id,
            ancestor.name,
            ancestor.ts,
            ancestor.dur,
            ancestor.track_id,
            'choreographer_parent' as relation
          FROM ancestor_slice(${ctx.sliceId}) ancestor
          WHERE ancestor.name GLOB 'Choreographer#doFrame*'
        `,
          schema: RELATED_EVENT_SCHEMA,
        }),
      );
    }

    if (ctx.name.startsWith('Choreographer#doFrame')) {
      datasets.push(
        new SourceDataset({
          src: `
          SELECT
            s.id, s.name, s.ts, s.dur, s.track_id, 'descendant_input' as relation
          FROM descendant_slice(${ctx.sliceId}) ds
          JOIN slice s ON ds.id = s.id
          WHERE s.name GLOB 'deliverInputEvent src*'
        `,
          schema: RELATED_EVENT_SCHEMA,
        }),
      );
    }

    return datasets;
  }
}

export const ANDROID_INPUT_RULES: RelationRule[] = [
  new InputIdRule(),
  new InputSequenceRule(),
  new InputConsumerRule(),
  new ChoreographerRule(),
];
