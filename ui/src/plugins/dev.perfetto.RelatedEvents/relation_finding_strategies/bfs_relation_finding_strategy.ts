import {RelatedEvent} from '..';
import {Trace} from '../../../public/trace';
import {
  Dataset,
  SourceDataset,
  UnionDatasetWithLineage,
} from '../../../trace_processor/dataset';
import {
  LONG,
  NUM,
  STR,
  STR_NULL,
  UNKNOWN,
} from '../../../trace_processor/query_result';
import {
  EventContext,
  RELATED_EVENT_SCHEMA,
  RELATION_SCHEMA,
  RelationFindingStrategy,
  RelationRule,
} from '../relation_finding_strategy';
import {Time, time, duration} from '../../../base/time';

interface DetailedEventInfo {
  eventName: string;
  eventTs: time;
  eventDur: duration;
  trackId: number;
  eventArgs: Map<string, string>;
}

const EVENT_DETAILS_SCHEMA = {
  id: NUM,
  name: STR,
  ts: LONG,
  dur: LONG,
  track_id: NUM,
  arg_set_id: NUM,
};

export class RuleBasedBfsStrategy implements RelationFindingStrategy {
  constructor(private rules: RelationRule[]) {}

  async findRelatedEvents(trace: Trace): Promise<Dataset | undefined> {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') {
      return undefined;
    }

    const initialEventId = Number(selection.eventId);
    if (isNaN(initialEventId)) {
      return undefined;
    }

    const initialTrack = trace.tracks.getTrack(selection.trackUri);
    if (!initialTrack) {
      return undefined;
    }

    const initialDetailsMap = await this.getEventDetailsBatched(
      trace,
      [initialEventId],
      initialTrack.renderer.getDataset?.(),
    );
    const initialEventDetails = initialDetailsMap.get(initialEventId);

    if (!initialEventDetails) {
      return undefined;
    }

    const queue: number[] = [initialEventId];
    const visitedEventIds: Set<number> = new Set([initialEventId]);
    const eventDetailsCache = new Map<number, DetailedEventInfo>();
    eventDetailsCache.set(initialEventId, initialEventDetails);

    const relatedEventDatasets: Set<Dataset> = new Set();
    const initialDataset = this.getEventDataset(
      trace,
      initialEventDetails.trackId,
    );

    if (initialDataset) {
      relatedEventDatasets.add(initialDataset);
    }

    let iterations = 0;
    while (queue.length > 0) {
      // TODO(ivankc) Investigate effect of high iteration count
      if (iterations++ > 200) {
        console.warn('RuleBasedBfsStrategy: Exceeded maximum traversals');
        break;
      }
      const currentEventId = queue.shift()!;
      const currentEventDetails = eventDetailsCache.get(currentEventId);

      if (!currentEventDetails) continue;

      const context: EventContext = {
        sliceId: currentEventId,
        name: currentEventDetails.eventName,
        args: currentEventDetails.eventArgs,
      };

      const directlyRelated = await this.getDirectlyRelatedEvents(
        trace,
        context,
      );
      const newEvents = directlyRelated.filter(
        (r) => !visitedEventIds.has(r.id),
      );

      // Mark as visited and add to queue immediately
      for (const related of newEvents) {
        visitedEventIds.add(related.id);
        queue.push(related.id);
      }

      // Group by track to batch-fetch details
      const eventsByTrack = new Map<number, number[]>();
      for (const related of newEvents) {
        if (!eventsByTrack.has(related.track_id)) {
          eventsByTrack.set(related.track_id, []);
        }
        eventsByTrack.get(related.track_id)!.push(related.id);
      }

      // Fetch details per track in batches
      for (const [trackId, eventIds] of eventsByTrack) {
        const relatedDataset = this.getEventDataset(trace, trackId);
        if (relatedDataset) {
          relatedEventDatasets.add(relatedDataset);
          const detailsMap = await this.getEventDetailsBatched(
            trace,
            eventIds,
            relatedDataset,
          );
          for (const [id, details] of detailsMap) {
            eventDetailsCache.set(id, details);
          }
        }
      }
    }

    if (relatedEventDatasets.size === 0) {
      return undefined;
    }

    const unionRelatedEventsDataset = UnionDatasetWithLineage.create([
      ...relatedEventDatasets,
    ]);

    const sql = unionRelatedEventsDataset.query(RELATION_SCHEMA);

    const finalDataset = new SourceDataset({
      src: sql,
      schema: RELATION_SCHEMA,
      filter: {
        col: 'id',
        in: [...visitedEventIds],
      },
    });

    return finalDataset;
  }

  private getEventDataset(trace: Trace, trackId: number): Dataset | undefined {
    const track = trace.tracks.findTrack((t) =>
      t.tags?.trackIds?.includes(trackId),
    );
    const baseDataset = track?.renderer.getDataset?.();
    return baseDataset;
  }

  private async getDirectlyRelatedEvents(
    trace: Trace,
    context: EventContext,
  ): Promise<RelatedEvent[]> {
    const relatedDatasets: Dataset[] = this.rules.flatMap((rule) =>
      rule.getRelatedEventsAsDataset(context),
    );
    if (relatedDatasets.length === 0) return [];

    const unionDataset = UnionDatasetWithLineage.create(relatedDatasets);
    const querySchema = {
      ...RELATED_EVENT_SCHEMA,
      __groupid: NUM,
      __partition: UNKNOWN,
    };
    const sql = unionDataset.query(querySchema);

    try {
      const result = await trace.engine.query(sql);
      const relatedEvents: RelatedEvent[] = [];
      const it = result.iter(RELATED_EVENT_SCHEMA);
      while (it.valid()) {
        relatedEvents.push({
          id: Number(it.id),
          name: it.name,
          ts: Time.fromRaw(it.ts),
          dur: Time.fromRaw(it.dur),
          track_id: Number(it.track_id),
        });
        it.next();
      }
      return relatedEvents;
    } catch (e) {
      return [];
    }
  }

  private async getEventDetailsBatched(
    trace: Trace,
    sliceIds: number[],
    dataset?: Dataset,
  ): Promise<Map<number, DetailedEventInfo>> {
    if (!dataset || sliceIds.length === 0) return new Map();

    const trackBaseQuery = dataset.query(EVENT_DETAILS_SCHEMA);
    const idList = sliceIds.join(',');
    const sql = `
        SELECT
            b.id,
            b.name,
            b.ts, 
            b.dur,
            b.track_id,
            args.key AS arg_key, 
            args.display_value AS arg_value
        FROM (${trackBaseQuery}) b
        LEFT JOIN args ON b.arg_set_id = args.arg_set_id
        WHERE b.id IN (${idList})
        `;

    try {
      const result = await trace.engine.query(sql);
      if (result.numRows() === 0) return new Map();

      const it = result.iter({
        id: NUM,
        name: STR,
        ts: LONG,
        dur: LONG,
        track_id: NUM,
        arg_key: STR_NULL,
        arg_value: STR_NULL,
      });
      const eventsMap = new Map<number, DetailedEventInfo>();

      while (it.valid()) {
        const id = it.id;
        let info = eventsMap.get(id);
        if (!info) {
          info = {
            eventName: it.name,
            eventTs: Time.fromRaw(it.ts),
            eventDur: Time.fromRaw(it.dur),
            trackId: it.track_id,
            eventArgs: new Map<string, string>(),
          };
          eventsMap.set(id, info);
        }

        if (it.arg_key && it.arg_value) {
          info.eventArgs.set(it.arg_key, it.arg_value);
        }
        it.next();
      }
      return eventsMap;
    } catch (e) {
      return new Map();
    }
  }
}
