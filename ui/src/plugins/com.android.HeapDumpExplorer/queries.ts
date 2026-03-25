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

import {Engine} from '../../trace_processor/engine';
import {
  BLOB_NULL,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
  type QueryResult,
} from '../../trace_processor/query_result';
import type {
  OverviewData,
  HeapInfo,
  InstanceRow,
  InstanceDetail,
  PrimOrRef,
  BitmapListRow,
  StringListRow,
  DuplicateBitmapGroup,
  DuplicateStringGroup,
  ClassRow,
} from './types';
import {fmtHex} from './format';
import {shortClassName, SQL_PREAMBLE} from './components';

async function requireDominatorTree(engine: Engine): Promise<void> {
  await engine.query(SQL_PREAMBLE);
}

function className(name: string | null, deobfuscated: string | null): string {
  return deobfuscated ?? name ?? '???';
}

function makeDisplay(cls: string, id: number): string {
  return `${shortClassName(cls)} ${fmtHex(id)}`;
}

function sqlEsc(s: string): string {
  return s.replace(/'/g, "''");
}

function heapFilter(heap: string | null): string {
  return heap ? `AND o.heap_type = '${sqlEsc(heap)}'` : '';
}

const KIND_TO_REACHABILITY: Record<string, string> = {
  KIND_WEAK_REFERENCE: 'weak',
  KIND_SOFT_REFERENCE: 'soft',
  KIND_PHANTOM_REFERENCE: 'phantom',
  KIND_FINALIZER_REFERENCE: 'finalizer',
};

function rowFromIter(it: {
  id: number;
  cls: string;
  deob: string | null;
  self_size: number;
  native_size: number;
  heap_type: string | null;
  root_type: string | null;
  dominated_size: number | null;
  dominated_native: number | null;
  dominated_obj_count: number | null;
  value_string: string | null;
  class_kind?: string;
}): InstanceRow {
  const cls = className(it.cls, it.deob);
  const retainedJava = it.dominated_size ?? it.self_size;
  const retainedNative = it.dominated_native ?? it.native_size;
  const heap = it.heap_type ?? 'default';
  const reachabilityName =
    (it.class_kind && KIND_TO_REACHABILITY[it.class_kind]) ?? 'strong';
  return {
    id: it.id,
    display: makeDisplay(cls, it.id),
    className: cls,
    isRoot: it.root_type !== null,
    rootTypeNames: it.root_type !== null ? [it.root_type] : null,
    reachabilityName,
    heap,
    shallowJava: it.self_size,
    shallowNative: it.native_size,
    retainedTotal: retainedJava + retainedNative,
    retainedCount: it.dominated_obj_count ?? 1,
    reachableSize: null,
    reachableNative: null,
    reachableCount: null,
    retainedByHeap: [{heap, java: retainedJava, native_: retainedNative}],
    str: it.value_string,
    referent: null,
  };
}

const INSTANCE_COLS = `
  o.id, c.name AS cls, c.deobfuscated_name AS deob,
  o.self_size, o.native_size, o.heap_type, o.root_type,
  d.dominated_size_bytes AS dominated_size,
  d.dominated_native_size_bytes AS dominated_native,
  d.dominated_obj_count,
  od.value_string, c.kind AS class_kind`;

const INSTANCE_ITER_SPEC = {
  id: NUM,
  cls: STR,
  deob: STR_NULL,
  self_size: NUM,
  native_size: NUM,
  heap_type: STR_NULL,
  root_type: STR_NULL,
  dominated_size: NUM_NULL,
  dominated_native: NUM_NULL,
  dominated_obj_count: NUM_NULL,
  value_string: STR_NULL,
  class_kind: STR,
};

function collectRows(res: QueryResult): InstanceRow[] {
  const rows: InstanceRow[] = [];
  for (const it = res.iter(INSTANCE_ITER_SPEC); it.valid(); it.next()) {
    rows.push(rowFromIter(it));
  }
  return rows;
}

/**
 * Batch-fetch content hashes for bitmap pixel buffers via DumpData.
 * Uses the pre-computed array_data_hash column from the trace processor,
 * avoiding expensive byte-level hashing in TypeScript.
 */
async function batchBitmapBufferHashes(
  engine: Engine,
  bitmaps: Array<{objectId: number; nativePtr: bigint}>,
): Promise<Map<number, string>> {
  const result = new Map<number, string>();
  const dumpData = await loadBitmapDumpData(engine);
  if (!dumpData) return result;

  // Build bufferObjId → [bitmapObjectId, ...] mapping.
  const bufToBitmaps = new Map<number, number[]>();
  for (const b of bitmaps) {
    const bufId = dumpData.bufferMap.get(b.nativePtr);
    if (bufId === undefined) continue;
    const existing = bufToBitmaps.get(bufId);
    if (existing) {
      existing.push(b.objectId);
    } else {
      bufToBitmaps.set(bufId, [b.objectId]);
    }
  }
  if (bufToBitmaps.size === 0) return result;

  const ids = [...bufToBitmaps.keys()].join(',');
  const bufRes = await engine.query(`
    SELECT o.id AS buf_id, od.array_data_hash AS hash
    FROM heap_graph_object o
    JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.id IN (${ids})
      AND od.array_data_hash IS NOT NULL
  `);
  for (
    const it = bufRes.iter({buf_id: NUM, hash: LONG_NULL});
    it.valid();
    it.next()
  ) {
    if (it.hash === null) continue;
    const hashStr = it.hash.toString();
    const bmpIds = bufToBitmaps.get(it.buf_id);
    if (bmpIds) {
      for (const id of bmpIds) {
        result.set(id, hashStr);
      }
    }
  }
  return result;
}

export async function getOverview(engine: Engine): Promise<OverviewData> {
  const countRes = await engine.query(
    `SELECT count(*) as cnt FROM heap_graph_object WHERE reachable != 0`,
  );
  const instanceCount = countRes.iter({cnt: NUM}).cnt;

  const heapRes = await engine.query(`
    SELECT
      ifnull(heap_type, 'default') AS heap,
      SUM(self_size) AS java,
      SUM(native_size) AS native_
    FROM heap_graph_object
    WHERE reachable != 0
    GROUP BY heap
    ORDER BY heap
  `);
  const heaps: HeapInfo[] = [];
  for (
    const it = heapRes.iter({heap: STR, java: NUM, native_: NUM});
    it.valid();
    it.next()
  ) {
    heaps.push({name: it.heap, java: it.java, native_: it.native_});
  }

  // Duplicate bitmaps grouped by pixel content hash. Each bitmap's compressed
  // DumpData buffer is hashed to detect true content duplicates rather than
  // just matching on dimensions. Skipped for proto heap graphs (no HPROF data).
  const hasPrimitivesRes = await engine.query(
    `SELECT 1 FROM heap_graph_primitive LIMIT 1`,
  );
  const hasPrimitives = hasPrimitivesRes.iter({}).valid();
  const dupRes = hasPrimitives
    ? await engine.query(`
    SELECT
      o.id,
      MAX(CASE WHEN f.field_name GLOB '*mWidth' THEN f.int_value END) AS w,
      MAX(CASE WHEN f.field_name GLOB '*mHeight' THEN f.int_value END) AS h,
      MAX(CASE WHEN f.field_name GLOB '*mNativePtr' THEN f.long_value END)
        AS native_ptr,
      o.self_size + o.native_size AS total_bytes
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    LEFT JOIN heap_graph_primitive f ON f.field_set_id = od.field_set_id
    WHERE o.reachable != 0
      AND (c.name = 'android.graphics.Bitmap'
        OR c.deobfuscated_name = 'android.graphics.Bitmap')
    GROUP BY o.id
    HAVING w IS NOT NULL AND h IS NOT NULL
  `)
    : null;

  // Collect bitmap info and compute content hashes.
  const bitmapInfos: Array<{
    id: number;
    w: number;
    h: number;
    totalBytes: number;
    nativePtr: bigint | null;
  }> = [];
  if (dupRes !== null) {
    for (
      const it = dupRes.iter({
        id: NUM,
        w: NUM,
        h: NUM,
        native_ptr: LONG_NULL,
        total_bytes: NUM,
      });
      it.valid();
      it.next()
    ) {
      bitmapInfos.push({
        id: it.id,
        w: it.w,
        h: it.h,
        totalBytes: it.total_bytes,
        nativePtr: it.native_ptr,
      });
    }
  }

  const hashInputs = bitmapInfos
    .filter((b) => b.nativePtr !== null)
    .map((b) => ({objectId: b.id, nativePtr: b.nativePtr!}));
  const hashes =
    hashInputs.length > 0
      ? await batchBitmapBufferHashes(engine, hashInputs)
      : new Map<number, string>();

  const hashGroups = new Map<
    string,
    {w: number; h: number; cnt: number; total: number; min: number}
  >();
  for (const b of bitmapInfos) {
    const hash = hashes.get(b.id);
    if (!hash) continue;
    const existing = hashGroups.get(hash);
    if (existing) {
      existing.cnt++;
      existing.total += b.totalBytes;
      existing.min = Math.min(existing.min, b.totalBytes);
    } else {
      hashGroups.set(hash, {
        w: b.w,
        h: b.h,
        cnt: 1,
        total: b.totalBytes,
        min: b.totalBytes,
      });
    }
  }
  const duplicateBitmaps: DuplicateBitmapGroup[] = [];
  for (const [key, g] of hashGroups) {
    if (g.cnt < 2) continue;
    duplicateBitmaps.push({
      groupKey: key,
      width: g.w,
      height: g.h,
      count: g.cnt,
      totalBytes: g.total,
      wastedBytes: g.total - g.min,
    });
  }
  duplicateBitmaps.sort((a, b) => b.wastedBytes - a.wastedBytes);

  // Duplicate strings grouped by value. Only available for HPROF dumps
  // which populate heap_graph_object_data.value_string.
  const duplicateStrings: DuplicateStringGroup[] = [];
  if (hasPrimitives) {
    const strRes = await engine.query(`
      SELECT
        od.value_string AS value,
        COUNT(*) AS cnt,
        SUM(o.self_size) AS total_bytes,
        MIN(o.self_size) AS min_bytes
      FROM heap_graph_object o
      JOIN heap_graph_class c ON o.type_id = c.id
      LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
      WHERE o.reachable != 0
        AND od.value_string IS NOT NULL
        AND (c.name = 'java.lang.String'
          OR c.deobfuscated_name = 'java.lang.String')
      GROUP BY od.value_string
      HAVING cnt > 1
      ORDER BY total_bytes - min_bytes DESC
      LIMIT 100
    `);
    for (
      const it = strRes.iter({
        value: STR,
        cnt: NUM,
        total_bytes: NUM,
        min_bytes: NUM,
      });
      it.valid();
      it.next()
    ) {
      duplicateStrings.push({
        value: it.value,
        count: it.cnt,
        totalBytes: it.total_bytes,
        wastedBytes: it.total_bytes - it.min_bytes,
      });
    }
  }

  return {
    instanceCount,
    heaps,
    duplicateBitmaps:
      duplicateBitmaps.length > 0 ? duplicateBitmaps : undefined,
    duplicateStrings:
      duplicateStrings.length > 0 ? duplicateStrings : undefined,
    hasFieldValues: hasPrimitives,
  };
}

export async function getAllocations(
  engine: Engine,
  heap: string | null,
): Promise<ClassRow[]> {
  await requireDominatorTree(engine);
  const hf = heapFilter(heap);
  const res = await engine.query(`
    SELECT
      ifnull(c.deobfuscated_name, c.name) AS cls,
      COUNT(*) AS cnt,
      SUM(o.self_size) AS shallow,
      SUM(o.native_size) AS native_shallow,
      SUM(ifnull(d.dominated_size_bytes, o.self_size)) AS retained,
      SUM(ifnull(d.dominated_native_size_bytes, o.native_size))
        AS retained_native,
      SUM(ifnull(d.dominated_obj_count, 1)) AS retained_count,
      ifnull(o.heap_type, 'default') AS heap
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    WHERE o.reachable != 0
      ${hf}
    GROUP BY cls, heap
    ORDER BY retained DESC
  `);
  const rows: ClassRow[] = [];
  for (
    const it = res.iter({
      cls: STR,
      cnt: NUM,
      shallow: NUM,
      native_shallow: NUM,
      retained: NUM,
      retained_native: NUM,
      retained_count: NUM,
      heap: STR,
    });
    it.valid();
    it.next()
  ) {
    rows.push({
      className: it.cls,
      count: it.cnt,
      shallowSize: it.shallow,
      nativeSize: it.native_shallow,
      retainedSize: it.retained,
      retainedNativeSize: it.retained_native,
      retainedCount: it.retained_count,
      reachableSize: null,
      reachableNativeSize: null,
      reachableCount: null,
      heap: it.heap,
    });
  }
  return rows;
}

export async function getRooted(engine: Engine): Promise<InstanceRow[]> {
  await requireDominatorTree(engine);
  const res = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_dominator_tree d
    JOIN heap_graph_object o ON d.id = o.id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE d.idom_id IS NULL
    ORDER BY d.dominated_size_bytes + d.dominated_native_size_bytes DESC
  `);
  return collectRows(res);
}

type FieldEntry = {name: string; typeName: string; value: PrimOrRef};

/** Fetch primitive and reference field values for an object. */
async function fetchFieldValues(
  engine: Engine,
  refSetId: number | null,
  fieldSetId: number | null,
): Promise<FieldEntry[]> {
  const fields: FieldEntry[] = [];

  if (fieldSetId !== null) {
    const fRes = await engine.query(`
      SELECT field_name, field_type,
        bool_value, byte_value, char_value, short_value,
        int_value, long_value, float_value, double_value
      FROM heap_graph_primitive
      WHERE field_set_id = ${fieldSetId}
      ORDER BY field_name
    `);
    for (
      const fit = fRes.iter({
        field_name: STR,
        field_type: STR,
        bool_value: NUM_NULL,
        byte_value: NUM_NULL,
        char_value: NUM_NULL,
        short_value: NUM_NULL,
        int_value: NUM_NULL,
        long_value: LONG_NULL,
        float_value: NUM_NULL,
        double_value: NUM_NULL,
      });
      fit.valid();
      fit.next()
    ) {
      fields.push({
        name: fit.field_name,
        typeName: fit.field_type,
        value: primFieldValue(fit),
      });
    }
  }

  if (refSetId !== null) {
    const rRes = await engine.query(`
      SELECT
        ifnull(r.deobfuscated_field_name, r.field_name) AS fname,
        ifnull(ct.deobfuscated_name, r.field_type_name) AS ftype,
        r.owned_id,
        c2.name AS ref_cls,
        c2.deobfuscated_name AS ref_deob,
        od2.value_string AS ref_str,
        o2.self_size AS ref_self_size,
        o2.native_size AS ref_native_size,
        d2.dominated_size_bytes AS ref_dominated_size,
        d2.dominated_native_size_bytes AS ref_dominated_native
      FROM heap_graph_reference r
      LEFT JOIN heap_graph_class ct ON r.field_type_name = ct.name
      LEFT JOIN heap_graph_object o2 ON r.owned_id = o2.id
      LEFT JOIN heap_graph_class c2 ON o2.type_id = c2.id
      LEFT JOIN heap_graph_object_data od2 ON o2.object_data_id = od2.id
      LEFT JOIN heap_graph_dominator_tree d2 ON d2.id = o2.id
      WHERE r.reference_set_id = ${refSetId}
      ORDER BY fname
    `);
    for (
      const rit = rRes.iter({
        fname: STR,
        ftype: STR_NULL,
        owned_id: NUM_NULL,
        ref_cls: STR_NULL,
        ref_deob: STR_NULL,
        ref_str: STR_NULL,
        ref_self_size: NUM_NULL,
        ref_native_size: NUM_NULL,
        ref_dominated_size: NUM_NULL,
        ref_dominated_native: NUM_NULL,
      });
      rit.valid();
      rit.next()
    ) {
      if (rit.owned_id === null || rit.owned_id === 0) {
        fields.push({
          name: rit.fname,
          typeName: rit.ftype ?? '',
          value: {kind: 'prim', v: 'null'},
        });
      } else {
        const refCls = className(rit.ref_cls, rit.ref_deob);
        fields.push({
          name: rit.fname,
          typeName: rit.ftype ?? '',
          value: {
            kind: 'ref',
            id: rit.owned_id,
            display: makeDisplay(refCls, rit.owned_id),
            str: rit.ref_str,
            shallowJava: rit.ref_self_size ?? 0,
            shallowNative: rit.ref_native_size ?? 0,
            retainedJava: rit.ref_dominated_size ?? rit.ref_self_size ?? 0,
            retainedNative:
              rit.ref_dominated_native ?? rit.ref_native_size ?? 0,
          },
        });
      }
    }
  }

  return fields;
}

/** Fetch dominator-tree path from GC root to the given object. */
async function fetchPathFromRoot(
  engine: Engine,
  id: number,
): Promise<InstanceDetail['pathFromRoot']> {
  const pathRes = await engine.query(`
    WITH RECURSIVE path(obj_id, depth) AS (
      SELECT ${id}, 0
      UNION ALL
      SELECT d.idom_id, p.depth + 1
      FROM path p
      JOIN heap_graph_dominator_tree d ON d.id = p.obj_id
      WHERE d.idom_id IS NOT NULL AND p.depth < 100
    )
    SELECT
      p.obj_id AS id, p.depth,
      c.name AS cls, c.deobfuscated_name AS deob,
      o.self_size, o.native_size, o.heap_type, o.root_type,
      dt.dominated_size_bytes AS dominated_size,
      dt.dominated_native_size_bytes AS dominated_native,
      dt.dominated_obj_count,
      od.value_string, c.kind AS class_kind
    FROM path p
    JOIN heap_graph_object o ON o.id = p.obj_id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    LEFT JOIN heap_graph_dominator_tree dt ON dt.id = o.id
    ORDER BY p.depth DESC
  `);
  const entries: {row: InstanceRow; objId: number}[] = [];
  for (
    const pit = pathRes.iter({
      id: NUM,
      depth: NUM,
      cls: STR,
      deob: STR_NULL,
      self_size: NUM,
      native_size: NUM,
      heap_type: STR_NULL,
      root_type: STR_NULL,
      dominated_size: NUM_NULL,
      dominated_native: NUM_NULL,
      dominated_obj_count: NUM_NULL,
      value_string: STR_NULL,
      class_kind: STR,
    });
    pit.valid();
    pit.next()
  ) {
    entries.push({row: rowFromIter(pit), objId: pit.id});
  }

  const result: NonNullable<InstanceDetail['pathFromRoot']> = [];
  for (let i = 0; i < entries.length; i++) {
    let field = '';
    if (i < entries.length - 1) {
      const parentId = entries[i].objId;
      const childId = entries[i + 1].objId;
      const fRes = await engine.query(`
        SELECT ifnull(r.deobfuscated_field_name, r.field_name) AS fname
        FROM heap_graph_reference r
        JOIN heap_graph_object o ON r.reference_set_id = o.reference_set_id
        WHERE o.id = ${parentId} AND r.owned_id = ${childId}
        LIMIT 1
      `);
      const fit = fRes.iter({fname: STR});
      if (fit.valid()) {
        field = '.' + fit.fname;
      }
    }
    result.push({
      row: entries[i].row,
      field,
      isDominator: true,
    });
  }
  return result.length > 0 ? result : null;
}

export async function getInstance(
  engine: Engine,
  id: number,
): Promise<InstanceDetail | null> {
  await requireDominatorTree(engine);
  const objRes = await engine.query(`
    SELECT
      o.id,
      c.name AS cls,
      c.deobfuscated_name AS deob,
      c.kind AS class_kind,
      sc.name AS super_cls,
      o.self_size,
      o.native_size,
      o.heap_type,
      o.root_type,
      od.value_string,
      o.reference_set_id,
      od.field_set_id,
      od.array_element_type,
      od.array_data_id,
      d.dominated_size_bytes AS dominated_size,
      d.dominated_native_size_bytes AS dominated_native,
      d.dominated_obj_count
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_class sc ON c.superclass_id = sc.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.id = ${id}
  `);

  const oit = objRes.iter({
    id: NUM,
    cls: STR,
    deob: STR_NULL,
    class_kind: STR,
    super_cls: STR_NULL,
    self_size: NUM,
    native_size: NUM,
    heap_type: STR_NULL,
    root_type: STR_NULL,
    value_string: STR_NULL,
    reference_set_id: NUM_NULL,
    field_set_id: NUM_NULL,
    array_element_type: STR_NULL,
    array_data_id: NUM_NULL,
    dominated_size: NUM_NULL,
    dominated_native: NUM_NULL,
    dominated_obj_count: NUM_NULL,
  });
  if (!oit.valid()) return null;

  const fullClassName = className(oit.cls, oit.deob);
  const classKind = oit.class_kind;
  const superClassName = oit.super_cls;
  const refSetId = oit.reference_set_id;
  const fieldSetId = oit.field_set_id;
  const arrayDataId = oit.array_data_id;
  const arrayElementType = oit.array_element_type;

  // Class objects in the C++ parser are named "java.lang.Class<ClassName>".
  const isClassObj = fullClassName.startsWith('java.lang.Class<');
  const isArrayInstance = fullClassName.endsWith('[]');

  const reachabilityName = KIND_TO_REACHABILITY[classKind] ?? 'strong';

  const row = rowFromIter({...oit, class_kind: classKind});
  row.reachabilityName = reachabilityName;

  // Detect referent for Reference subclasses.
  if (reachabilityName !== 'strong' && refSetId !== null) {
    const refResult = await engine.query(`
      SELECT
        r.owned_id,
        c2.name AS ref_cls, c2.deobfuscated_name AS ref_deob,
        od2.value_string AS ref_str
      FROM heap_graph_reference r
      LEFT JOIN heap_graph_object o2 ON r.owned_id = o2.id
      LEFT JOIN heap_graph_class c2 ON o2.type_id = c2.id
      LEFT JOIN heap_graph_object_data od2 ON o2.object_data_id = od2.id
      WHERE r.reference_set_id = ${refSetId}
        AND (r.field_name GLOB '*referent'
          OR r.deobfuscated_field_name GLOB '*referent')

    `);
    const rit = refResult.iter({
      owned_id: NUM_NULL,
      ref_cls: STR_NULL,
      ref_deob: STR_NULL,
      ref_str: STR_NULL,
    });
    if (rit.valid() && rit.owned_id !== null && rit.owned_id !== 0) {
      const refCls = className(rit.ref_cls, rit.ref_deob);
      row.referent = {
        id: rit.owned_id,
        display: makeDisplay(refCls, rit.owned_id),
        className: refCls,
        isRoot: false,
        rootTypeNames: null,
        reachabilityName: 'strong',
        heap: row.heap,
        shallowJava: 0,
        shallowNative: 0,
        retainedTotal: 0,
        retainedCount: 1,
        reachableCount: null,
        reachableSize: null,
        reachableNative: null,
        retainedByHeap: [],
        str: rit.ref_str,
        referent: null,
      };
    }
  }

  // Look up the java.lang.Class<X> object for this class.
  let classObjRow: InstanceRow | null = null;
  {
    const classObjName = sqlEsc(`java.lang.Class<${oit.cls}>`);
    const cRes = await engine.query(`
      SELECT ${INSTANCE_COLS}
      FROM heap_graph_object o
      JOIN heap_graph_class c ON o.type_id = c.id
      LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
      LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
      WHERE (c.name = '${classObjName}'
        OR c.deobfuscated_name = '${classObjName}')

    `);
    const rows = collectRows(cRes);
    if (rows.length > 0) classObjRow = rows[0];
  }

  const instanceFields =
    !isArrayInstance && !isClassObj
      ? await fetchFieldValues(engine, refSetId, fieldSetId)
      : [];

  let arrayLength = 0;
  let elemTypeName: string | null = null;
  const arrayElems: InstanceDetail['arrayElems'] = [];
  if (isArrayInstance) {
    elemTypeName = fullClassName.slice(0, -2); // "int[]" → "int"
    if (arrayDataId !== null && arrayElementType !== null) {
      // Primitive array: decode via JSON SQL function.
      const jsonRes = await engine.query(`
        SELECT __intrinsic_heap_graph_array_json(${arrayDataId}) AS data
      `);
      const jit = jsonRes.iter({data: STR_NULL});
      if (jit.valid() && jit.data !== null) {
        const values = JSON.parse(jit.data) as Array<number | string | boolean>;
        for (let i = 0; i < values.length; i++) {
          arrayElems.push({
            idx: i,
            value: {
              kind: 'prim',
              v: formatPrimValue(arrayElementType, values[i]),
            },
          });
        }
        arrayLength = values.length;
      }
    } else if (refSetId !== null) {
      // Object array elements (String[], Object[], etc.)
      // For HPROF, field_name is "[0]", "[1]", etc.
      // For perfetto heap graph, field_name may be empty or a plain name.
      // We use the row order as fallback index.
      const oaRes = await engine.query(`
        SELECT
          r.field_name AS fname,
          r.owned_id,
          c2.name AS ref_cls,
          c2.deobfuscated_name AS ref_deob,
          od2.value_string AS ref_str,
          o2.self_size AS ref_shallow,
          o2.native_size AS ref_native,
          ifnull(d2.dominated_size_bytes, o2.self_size) AS ref_retained,
          ifnull(d2.dominated_native_size_bytes, o2.native_size)
            AS ref_retained_native
        FROM heap_graph_reference r
        LEFT JOIN heap_graph_object o2 ON r.owned_id = o2.id
        LEFT JOIN heap_graph_object_data od2 ON o2.object_data_id = od2.id
        LEFT JOIN heap_graph_class c2 ON o2.type_id = c2.id
        LEFT JOIN heap_graph_dominator_tree d2 ON d2.id = o2.id
        WHERE r.reference_set_id = ${refSetId}
        ORDER BY r.id
      `);
      let seqIdx = 0;
      for (
        const oait = oaRes.iter({
          fname: STR,
          owned_id: NUM_NULL,
          ref_cls: STR_NULL,
          ref_deob: STR_NULL,
          ref_str: STR_NULL,
          ref_shallow: NUM_NULL,
          ref_native: NUM_NULL,
          ref_retained: NUM_NULL,
          ref_retained_native: NUM_NULL,
        });
        oait.valid();
        oait.next()
      ) {
        const raw = oait.fname;
        let idx: number;
        if (raw.startsWith('[') && raw.endsWith(']')) {
          idx = parseInt(raw.slice(1, raw.length - 1), 10);
        } else {
          const parsed = parseInt(raw, 10);
          idx = Number.isNaN(parsed) ? seqIdx : parsed;
        }
        seqIdx++;
        let value: PrimOrRef;
        if (oait.owned_id === null || oait.owned_id === 0) {
          value = {kind: 'prim', v: 'null'};
        } else {
          const refCls = className(oait.ref_cls, oait.ref_deob);
          value = {
            kind: 'ref',
            id: oait.owned_id,
            display: makeDisplay(refCls, oait.owned_id),
            str: oait.ref_str,
            shallowJava: oait.ref_shallow ?? 0,
            shallowNative: oait.ref_native ?? 0,
            retainedJava: oait.ref_retained ?? 0,
            retainedNative: oait.ref_retained_native ?? 0,
          };
        }
        arrayElems.push({idx, value});
        arrayLength = Math.max(arrayLength, idx + 1);
      }
    }
  }

  const revRes = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_reference r
    JOIN heap_graph_object o ON r.owner_id = o.id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE r.owned_id = ${id}
    ORDER BY (ifnull(d.dominated_size_bytes, 0)
      + ifnull(d.dominated_native_size_bytes, 0)) DESC
  `);
  const reverseRefs = collectRows(revRes);

  const domRes = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_dominator_tree d
    JOIN heap_graph_object o ON d.id = o.id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE d.idom_id = ${id}
    ORDER BY (d.dominated_size_bytes + d.dominated_native_size_bytes) DESC
  `);
  const dominated = collectRows(domRes);

  const pathFromRoot = await fetchPathFromRoot(engine, id);

  const staticFields = isClassObj
    ? await fetchFieldValues(engine, refSetId, fieldSetId)
    : [];

  let bitmap: InstanceDetail['bitmap'] = null;
  if (fullClassName === 'android.graphics.Bitmap') {
    bitmap = await extractBitmapPixels(engine, fieldSetId);
  }
  if (bitmap === null) {
    const deobName = className(oit.cls, oit.deob);
    if (deobName === 'android.graphics.Bitmap') {
      bitmap = await extractBitmapPixels(engine, fieldSetId);
    }
  }

  // Resolve superclass object id (the java.lang.Class<Super> heap object).
  let superClassObjId: number | null = null;
  if (superClassName !== null) {
    const superObjName = `java.lang.Class<${superClassName}>`.replace(
      /'/g,
      "''",
    );
    const superRes = await engine.query(`
      SELECT o.id
      FROM heap_graph_object o
      JOIN heap_graph_class c ON o.type_id = c.id
      WHERE (c.name = '${superObjName}'
        OR c.deobfuscated_name = '${superObjName}')

    `);
    const sit = superRes.iter({id: NUM});
    if (sit.valid()) superClassObjId = sit.id;
  }

  // Extract forClassName for class objects: "java.lang.Class<Foo>" → "Foo"
  let forClassName: string | null = null;
  if (isClassObj) {
    const match = fullClassName.match(/^java\.lang\.Class<(.+)>$/);
    forClassName = match ? match[1] : fullClassName;
  }

  return {
    row,
    isClassObj,
    isArrayInstance,
    isClassInstance: !isClassObj && !isArrayInstance,
    classObjRow,
    forClassName,
    superClassObjId,
    instanceSize: row.shallowJava,
    staticFields,
    instanceFields,
    elemTypeName,
    arrayLength,
    arrayElems,
    bitmap,
    reverseRefs,
    dominated,
    pathFromRoot,
  };
}

export async function getRawArrayBlob(
  engine: Engine,
  objectId: number,
): Promise<Uint8Array | null> {
  const res = await engine.query(`
    SELECT __intrinsic_heap_graph_array(od.array_data_id) AS data
    FROM heap_graph_object o
    JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.id = ${objectId} AND od.array_data_id IS NOT NULL
  `);
  const it = res.iter({data: BLOB_NULL});
  if (it.valid() && it.data !== null) return it.data;
  return null;
}

export async function getRawBitmapBlob(
  engine: Engine,
  objectId: number,
): Promise<{data: Uint8Array; format: string} | null> {
  // Read mNativePtr from the Bitmap instance fields.
  const fieldRes = await engine.query(`
    SELECT f.long_value
    FROM heap_graph_object o
    JOIN heap_graph_object_data od ON o.object_data_id = od.id
    JOIN heap_graph_primitive f ON f.field_set_id = od.field_set_id
    WHERE o.id = ${objectId}
      AND f.field_name GLOB '*mNativePtr'
  `);
  const fit = fieldRes.iter({long_value: LONG_NULL});
  if (!fit.valid() || fit.long_value === null) return null;
  const nativePtr = fit.long_value;

  const dumpData = await loadBitmapDumpData(engine);
  if (!dumpData) return null;
  const bufferObjId = dumpData.bufferMap.get(nativePtr);
  if (bufferObjId === undefined) return null;

  const bufRes = await engine.query(`
    SELECT __intrinsic_heap_graph_array(od.array_data_id) AS data
    FROM heap_graph_object o
    JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.id = ${bufferObjId}
  `);
  const bit = bufRes.iter({data: BLOB_NULL});
  if (!bit.valid() || bit.data === null) return null;

  const format = DUMP_DATA_FORMAT_NAMES[dumpData.format] ?? 'png';
  return {data: bit.data, format};
}

/** Format a single JSON-decoded primitive value for display. */
function formatPrimValue(type: string, v: number | string | boolean): string {
  if (type === 'boolean') return Number(v) !== 0 ? 'true' : 'false';
  if (type === 'char') {
    const c = v as number;
    return c >= 32 && c < 127 ? `'${String.fromCharCode(c)}'` : String(c);
  }
  return String(v);
}

//
// Modern Android (API 26+) stores bitmap pixel data via a static field
// `android.graphics.Bitmap.dumpData` pointing to a `Bitmap$DumpData` instance.
// DumpData contains: format (int: 0=JPEG, 1=PNG, 2-4=WEBP), natives (long[])
// mapping native pointers, and buffers (Object[] of byte[]) with compressed
// image data. Each Bitmap instance has mNativePtr (long) used to index into
// the natives/buffers arrays.

interface BitmapDumpData {
  format: number; // 0=JPEG, 1=PNG, 2-4=WEBP
  // Map from native pointer (as bigint) to buffer object ID.
  bufferMap: Map<bigint, number>;
}

let cachedDumpData: BitmapDumpData | null | undefined;

export function resetBitmapDumpDataCache(): void {
  cachedDumpData = undefined;
}

async function loadBitmapDumpData(
  engine: Engine,
): Promise<BitmapDumpData | null> {
  if (cachedDumpData !== undefined) return cachedDumpData;

  // Step 1: Find the Bitmap class object.
  const classObjRes = await engine.query(`
    SELECT o.reference_set_id
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    WHERE (c.name LIKE '%Class<android.graphics.Bitmap>'
      OR c.deobfuscated_name LIKE '%Class<android.graphics.Bitmap>')

  `);
  const classIt = classObjRes.iter({reference_set_id: NUM_NULL});
  if (!classIt.valid() || classIt.reference_set_id === null) {
    cachedDumpData = null;
    return null;
  }

  // Step 2: Follow dumpData reference from the class object.
  const ddRes = await engine.query(`
    SELECT r.owned_id AS dump_data_id
    FROM heap_graph_reference r
    WHERE r.reference_set_id = ${classIt.reference_set_id}
      AND r.field_name GLOB '*dumpData'

  `);
  const ddIt = ddRes.iter({dump_data_id: NUM_NULL});
  if (!ddIt.valid() || ddIt.dump_data_id === null) {
    cachedDumpData = null;
    return null;
  }
  const dumpDataId = ddIt.dump_data_id;

  // Step 3: Read format from DumpData's fields.
  const fmtRes = await engine.query(`
    SELECT int_value FROM heap_graph_primitive
    WHERE field_set_id = (
      SELECT od.field_set_id FROM heap_graph_object o
      JOIN heap_graph_object_data od ON o.object_data_id = od.id
      WHERE o.id = ${dumpDataId}
    )
      AND field_name GLOB '*format'

  `);
  const fmtIt = fmtRes.iter({int_value: NUM_NULL});
  const format = fmtIt.valid() ? fmtIt.int_value ?? 1 : 1;

  // Step 4: Get DumpData's references — natives (long[]) and buffers (Object[]).
  const refsRes = await engine.query(`
    SELECT r.field_name, r.owned_id
    FROM heap_graph_reference r
    WHERE r.reference_set_id = (SELECT reference_set_id FROM heap_graph_object WHERE id = ${dumpDataId})
      AND (r.field_name GLOB '*natives' OR r.field_name GLOB '*buffers')
  `);
  let nativesObjId: number | null = null;
  let buffersObjId: number | null = null;
  for (
    const it = refsRes.iter({field_name: STR, owned_id: NUM});
    it.valid();
    it.next()
  ) {
    if (it.field_name.endsWith('natives')) nativesObjId = it.owned_id;
    if (it.field_name.endsWith('buffers')) buffersObjId = it.owned_id;
  }
  if (nativesObjId === null || buffersObjId === null) {
    cachedDumpData = null;
    return null;
  }

  // Step 5: Decode natives long[] via JSON.
  const nativesRes = await engine.query(`
    SELECT __intrinsic_heap_graph_array_json(od.array_data_id) AS data
    FROM heap_graph_object o
    JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.id = ${nativesObjId}
  `);
  const nativesIt = nativesRes.iter({data: STR_NULL});
  if (!nativesIt.valid() || nativesIt.data === null) {
    cachedDumpData = null;
    return null;
  }
  // Native pointers need BigInt for 64-bit precision in Map lookups.
  const nativesPtrs = (JSON.parse(nativesIt.data) as string[]).map(BigInt);

  // Step 6: Get buffers Object[] references — array index → byte[] object ID.
  const bufsRes = await engine.query(`
    SELECT r.field_name, r.owned_id
    FROM heap_graph_reference r
    WHERE r.reference_set_id = (SELECT reference_set_id FROM heap_graph_object WHERE id = ${buffersObjId})
    ORDER BY CAST(SUBSTR(r.field_name, 2) AS INTEGER)
  `);
  const bufferObjIds: number[] = [];
  for (
    const it = bufsRes.iter({field_name: STR, owned_id: NUM});
    it.valid();
    it.next()
  ) {
    // field_name is "[0]", "[1]", etc.
    const idx = parseInt(it.field_name.slice(1, -1), 10);
    bufferObjIds[idx] = it.owned_id;
  }

  // Step 7: Build nativePtr → buffer object ID map.
  const bufferMap = new Map<bigint, number>();
  const count = Math.min(nativesPtrs.length, bufferObjIds.length);
  for (let i = 0; i < count; i++) {
    if (bufferObjIds[i] !== undefined && bufferObjIds[i] !== 0) {
      bufferMap.set(nativesPtrs[i], bufferObjIds[i]);
    }
  }

  cachedDumpData = {format, bufferMap};
  return cachedDumpData;
}

const DUMP_DATA_FORMAT_NAMES: Record<number, string> = {
  0: 'jpeg',
  1: 'png',
  2: 'webp',
  3: 'webp',
  4: 'webp',
};

async function extractBitmapPixels(
  engine: Engine,
  fieldSetId: number | null,
): Promise<InstanceDetail['bitmap']> {
  if (fieldSetId === null) return null;

  const dimRes = await engine.query(`
    SELECT field_name, int_value, long_value
    FROM heap_graph_primitive
    WHERE field_set_id = ${fieldSetId}
      AND (field_name GLOB '*mWidth' OR field_name GLOB '*mHeight'
        OR field_name GLOB '*mNativePtr')
  `);
  let width = 0;
  let height = 0;
  let nativePtr = 0n;
  for (
    const it = dimRes.iter({
      field_name: STR,
      int_value: NUM_NULL,
      long_value: LONG_NULL,
    });
    it.valid();
    it.next()
  ) {
    if (it.field_name.endsWith('mWidth')) width = it.int_value ?? 0;
    if (it.field_name.endsWith('mHeight')) height = it.int_value ?? 0;
    if (it.field_name.endsWith('mNativePtr')) {
      nativePtr = it.long_value ?? 0n;
    }
  }
  if (width <= 0 || height <= 0 || nativePtr === 0n) return null;

  const dumpData = await loadBitmapDumpData(engine);
  if (dumpData === null) return null;

  const bufferObjId = dumpData.bufferMap.get(nativePtr);
  if (bufferObjId === undefined) return null;

  const bufRes = await engine.query(`
    SELECT __intrinsic_heap_graph_array(od.array_data_id) AS data
    FROM heap_graph_object o
    JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.id = ${bufferObjId}
  `);
  const bufIt = bufRes.iter({data: BLOB_NULL});
  if (!bufIt.valid() || bufIt.data === null) return null;

  const format = DUMP_DATA_FORMAT_NAMES[dumpData.format] ?? 'png';
  return {width, height, format, data: bufIt.data};
}

export async function getBitmapPixels(
  engine: Engine,
  objectId: number,
): Promise<InstanceDetail['bitmap']> {
  const res = await engine.query(`
    SELECT od.field_set_id
    FROM heap_graph_object o
    JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.id = ${objectId}
`);
  const row = res.maybeFirstRow({field_set_id: NUM_NULL});
  return extractBitmapPixels(engine, row?.field_set_id ?? null);
}

export async function search(
  engine: Engine,
  query: string,
): Promise<InstanceRow[]> {
  await requireDominatorTree(engine);
  if (query.startsWith('0x') || query.startsWith('0X')) {
    const numId = parseInt(query, 16);
    if (!isNaN(numId)) {
      const res = await engine.query(`
        SELECT ${INSTANCE_COLS}
        FROM heap_graph_object o
        JOIN heap_graph_class c ON o.type_id = c.id
        LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
        LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
        WHERE o.id = ${numId}
      `);
      return collectRows(res);
    }
  }

  const escaped = query.replace(/[%_\\]/g, '\\$&').replace(/'/g, "''");
  const res = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.reachable != 0
      AND (c.name LIKE '%${escaped}%' ESCAPE '\\'
        OR c.deobfuscated_name LIKE '%${escaped}%' ESCAPE '\\')
    ORDER BY (ifnull(d.dominated_size_bytes, 0)
      + ifnull(d.dominated_native_size_bytes, 0)) DESC
  `);
  return collectRows(res);
}

export async function getObjects(
  engine: Engine,
  cls: string,
  heap: string | null,
): Promise<InstanceRow[]> {
  await requireDominatorTree(engine);
  const escaped = sqlEsc(cls);
  const hf = heapFilter(heap);
  const res = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    WHERE o.reachable != 0
      AND (c.name = '${escaped}' OR c.deobfuscated_name = '${escaped}')
      ${hf}
    ORDER BY o.self_size + o.native_size DESC
  `);
  return collectRows(res);
}

export async function getObjectsByFlamegraphSelection(
  engine: Engine,
  pathHashes: string,
  isDominator: boolean,
): Promise<InstanceRow[]> {
  await requireDominatorTree(engine);
  // Query objects matching the given path hashes from the flamegraph.
  // Path hashes are comma-separated integers identifying class tree nodes.
  const hashTable = isDominator
    ? '_heap_graph_dominator_path_hashes'
    : '_heap_graph_path_hashes';
  const values = pathHashes
    .split(',')
    .map((v) => `(${v.trim()})`)
    .join(', ');
  const res = await engine.query(`
    WITH _hde_sel(path_hash) AS (VALUES ${values})
    SELECT ${INSTANCE_COLS}
    FROM _hde_sel f
    JOIN ${hashTable} h ON h.path_hash = f.path_hash
    JOIN heap_graph_object o ON o.id = h.id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    ORDER BY o.self_size + o.native_size DESC
  `);
  return collectRows(res);
}

export async function getHeapGraphTrackInfo(
  engine: Engine,
  objectId: number,
  isDominator?: boolean,
): Promise<{
  upid: number;
  eventId: number;
  className: string | null;
  pathHash: string | null;
  pathIsDominator: boolean | null;
} | null> {
  const res = await engine.query(`
    SELECT
      o.upid,
      c.name AS class_name,
      (SELECT MIN(e.id)
       FROM heap_graph_object e
       WHERE e.upid = o.upid
         AND e.graph_sample_ts = o.graph_sample_ts) AS event_id
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    WHERE o.id = ${objectId}
  `);
  const row = res.maybeFirstRow({
    upid: NUM,
    event_id: NUM,
    class_name: STR_NULL,
  });
  if (!row) return null;

  // Look up path hash. When isDominator is known, only check the matching
  // table so the flamegraph navigates back to the correct metric.
  const tables =
    isDominator === true
      ? [{table: '_heap_graph_dominator_path_hashes', isDom: true}]
      : isDominator === false
        ? [{table: '_heap_graph_path_hashes', isDom: false}]
        : [
            {table: '_heap_graph_dominator_path_hashes', isDom: true},
            {table: '_heap_graph_path_hashes', isDom: false},
          ];

  let pathHash: string | null = null;
  let pathIsDominator: boolean | null = null;
  for (const {table, isDom} of tables) {
    try {
      const ph = await engine.query(`
        SELECT CAST(path_hash AS TEXT) AS ph
        FROM ${table} WHERE id = ${objectId}
      `);
      const phRow = ph.maybeFirstRow({ph: STR_NULL});
      if (phRow?.ph) {
        pathHash = phRow.ph;
        pathIsDominator = isDom;
        break;
      }
    } catch (_) {
      // Table may not exist if the module hasn't been loaded yet.
    }
  }

  return {
    upid: row.upid,
    eventId: row.event_id,
    className: row.class_name,
    pathHash,
    pathIsDominator,
  };
}

export async function getStringList(engine: Engine): Promise<StringListRow[]> {
  await requireDominatorTree(engine);
  const res = await engine.query(`
    SELECT
      o.id,
      od.value_string AS value,
      o.self_size,
      o.native_size,
      o.heap_type,
      c.name AS cls,
      c.deobfuscated_name AS deob,
      d.dominated_size_bytes AS dominated_size,
      d.dominated_native_size_bytes AS dominated_native
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    WHERE o.reachable != 0
      AND od.value_string IS NOT NULL
      AND (c.name = 'java.lang.String'
        OR c.deobfuscated_name = 'java.lang.String')
    ORDER BY (ifnull(d.dominated_size_bytes, 0)
      + ifnull(d.dominated_native_size_bytes, 0)) DESC
  `);

  const rows: StringListRow[] = [];
  for (
    const it = res.iter({
      id: NUM,
      value: STR,
      self_size: NUM,
      native_size: NUM,
      heap_type: STR_NULL,
      cls: STR,
      deob: STR_NULL,
      dominated_size: NUM_NULL,
      dominated_native: NUM_NULL,
    });
    it.valid();
    it.next()
  ) {
    const fullCls = className(it.cls, it.deob);
    rows.push({
      id: it.id,
      value: it.value,
      length: it.value.length,
      retainedSize: it.dominated_size ?? it.self_size,
      reachableSize: null,
      reachableNativeSize: null,
      reachableCount: null,
      shallowSize: it.self_size,
      nativeSize: it.native_size,
      heap: it.heap_type ?? 'default',
      className: fullCls,
      display: makeDisplay(fullCls, it.id),
    });
  }
  return rows;
}

export async function getBitmapList(engine: Engine): Promise<BitmapListRow[]> {
  await requireDominatorTree(engine);
  const dumpData = await loadBitmapDumpData(engine);
  const res = await engine.query(`
    SELECT
      o.id,
      c.name AS cls,
      c.deobfuscated_name AS deob,
      o.self_size,
      o.native_size,
      o.heap_type,
      o.root_type,
      d.dominated_size_bytes AS dominated_size,
      d.dominated_native_size_bytes AS dominated_native,
      d.dominated_obj_count,
      od.value_string,
      c.kind AS class_kind,
      MAX(CASE WHEN f.field_name GLOB '*mWidth' THEN f.int_value END) AS width,
      MAX(CASE WHEN f.field_name GLOB '*mHeight' THEN f.int_value END) AS height,
      MAX(CASE WHEN f.field_name GLOB '*mDensity' THEN f.int_value END) AS density,
      MAX(CASE WHEN f.field_name GLOB '*mNativePtr' THEN f.long_value END) AS native_ptr
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON o.object_data_id = od.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_primitive f ON f.field_set_id = od.field_set_id
    WHERE o.reachable != 0
      AND (c.name = 'android.graphics.Bitmap'
        OR c.deobfuscated_name = 'android.graphics.Bitmap')
    GROUP BY o.id
    ORDER BY (ifnull(d.dominated_size_bytes, 0)
      + ifnull(d.dominated_native_size_bytes, 0)) DESC
  `);

  // Collect rows and native pointers for hash lookup.
  const rawRows: Array<{
    row: InstanceRow;
    w: number;
    h: number;
    hasPixelData: boolean;
    density: number;
    nativePtr: bigint | null;
  }> = [];
  const hashInputs: Array<{objectId: number; nativePtr: bigint}> = [];
  for (
    const it = res.iter({
      id: NUM,
      cls: STR,
      deob: STR_NULL,
      self_size: NUM,
      native_size: NUM,
      heap_type: STR_NULL,
      root_type: STR_NULL,
      dominated_size: NUM_NULL,
      dominated_native: NUM_NULL,
      dominated_obj_count: NUM_NULL,
      value_string: STR_NULL,
      class_kind: STR,
      width: NUM_NULL,
      height: NUM_NULL,
      density: NUM_NULL,
      native_ptr: LONG_NULL,
    });
    it.valid();
    it.next()
  ) {
    const w = it.width ?? 0;
    const h = it.height ?? 0;
    let hasPixelData = false;
    if (dumpData !== null && it.native_ptr !== null && w > 0 && h > 0) {
      hasPixelData = dumpData.bufferMap.has(it.native_ptr);
      if (hasPixelData) {
        hashInputs.push({objectId: it.id, nativePtr: it.native_ptr});
      }
    }
    rawRows.push({
      row: rowFromIter(it),
      w,
      h,
      hasPixelData,
      density: it.density ?? 0,
      nativePtr: it.native_ptr,
    });
  }

  // Look up pre-computed content hashes for bitmaps with pixel data.
  const hashes =
    hashInputs.length > 0
      ? await batchBitmapBufferHashes(engine, hashInputs)
      : new Map<number, string>();

  const rows: BitmapListRow[] = rawRows.map((r) => ({
    row: r.row,
    width: r.w,
    height: r.h,
    pixelCount: r.w * r.h,
    hasPixelData: r.hasPixelData,
    density: r.density,
    bufferHash: hashes.get(r.row.id) ?? null,
  }));
  return rows;
}

function primFieldValue(it: {
  field_type: string;
  bool_value: number | null;
  byte_value: number | null;
  char_value: number | null;
  short_value: number | null;
  int_value: number | null;
  long_value: bigint | null;
  float_value: number | null;
  double_value: number | null;
}): PrimOrRef {
  switch (it.field_type) {
    case 'boolean':
      return {
        kind: 'prim',
        v: it.bool_value !== null && it.bool_value !== 0 ? 'true' : 'false',
      };
    case 'byte':
      return {kind: 'prim', v: String(it.byte_value ?? 0)};
    case 'char': {
      const code = it.char_value ?? 0;
      const ch =
        code >= 32 && code < 127
          ? `'${String.fromCharCode(code)}'`
          : String(code);
      return {kind: 'prim', v: ch};
    }
    case 'short':
      return {kind: 'prim', v: String(it.short_value ?? 0)};
    case 'int':
      return {kind: 'prim', v: String(it.int_value ?? 0)};
    case 'long':
      return {kind: 'prim', v: String(it.long_value ?? 0)};
    case 'float':
      return {kind: 'prim', v: String(it.float_value ?? 0)};
    case 'double':
      return {kind: 'prim', v: String(it.double_value ?? 0)};
    default:
      return {kind: 'prim', v: '???'};
  }
}

//
// The _heap_graph_object_tree_aggregation table computes cumulative reachable
// sizes via a BFS tree.  The table materialisation is expensive on first access,
// so we load it asynchronously and fill in reachable columns after the initial
// render.  The module INCLUDE is cached per-engine via a WeakMap.

const objectTreeReady = new WeakMap<Engine, Promise<void>>();

function ensureObjectTree(engine: Engine): Promise<void> {
  let p = objectTreeReady.get(engine);
  if (!p) {
    p = engine
      .query(`INCLUDE PERFETTO MODULE android.memory.heap_graph.object_tree`)
      .then(() => {});
    objectTreeReady.set(engine, p);
  }
  return p;
}

/** Fetch reachable (cumulative) sizes for a set of object IDs. */
async function getReachableSizes(
  engine: Engine,
  ids: number[],
): Promise<Map<number, {size: number; native: number; count: number}>> {
  await ensureObjectTree(engine);
  if (ids.length === 0) return new Map();
  const res = await engine.query(`
    SELECT id,
      cumulative_size AS size,
      cumulative_native_size AS native_size,
      cumulative_count AS count
    FROM _heap_graph_object_tree_aggregation
    WHERE id IN (${ids.join(',')})
  `);
  const map = new Map<number, {size: number; native: number; count: number}>();
  for (
    const it = res.iter({id: NUM, size: NUM, native_size: NUM, count: NUM});
    it.valid();
    it.next()
  ) {
    map.set(it.id, {size: it.size, native: it.native_size, count: it.count});
  }
  return map;
}

/**
 * Enrich InstanceRow[] with reachable sizes.  Call after initial data load;
 * the caller should trigger a re-render when the returned promise resolves.
 */
export async function enrichWithReachable(
  engine: Engine,
  rows: InstanceRow[],
): Promise<void> {
  const unenriched = rows.filter((r) => r.reachableSize === null);
  if (unenriched.length === 0) return;
  const ids = unenriched.map((r) => r.id);
  const map = await getReachableSizes(engine, ids);
  for (const row of unenriched) {
    const s = map.get(row.id);
    row.reachableSize = s?.size ?? 0;
    row.reachableNative = s?.native ?? 0;
    row.reachableCount = s?.count ?? 0;
  }
}

/**
 * Enrich ClassRow[] with reachable sizes (summed per class+heap group).
 */
export async function enrichClassRowsWithReachable(
  engine: Engine,
  rows: ClassRow[],
  heap: string | null,
): Promise<void> {
  await ensureObjectTree(engine);
  const hf = heapFilter(heap);
  const res = await engine.query(`
    SELECT
      ifnull(c.deobfuscated_name, c.name) AS cls,
      SUM(a.cumulative_size) AS reachable,
      SUM(a.cumulative_native_size) AS reachable_native,
      SUM(a.cumulative_count) AS reachable_count,
      ifnull(o.heap_type, 'default') AS heap
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    JOIN _heap_graph_object_tree_aggregation a ON a.id = o.id
    WHERE o.reachable != 0
      ${hf}
    GROUP BY cls, heap
  `);
  const map = new Map<
    string,
    {reachable: number; reachableNative: number; reachableCount: number}
  >();
  for (
    const it = res.iter({
      cls: STR,
      reachable: NUM,
      reachable_native: NUM,
      reachable_count: NUM,
      heap: STR,
    });
    it.valid();
    it.next()
  ) {
    map.set(`${it.cls}\0${it.heap}`, {
      reachable: it.reachable,
      reachableNative: it.reachable_native,
      reachableCount: it.reachable_count,
    });
  }
  for (const row of rows) {
    const s = map.get(`${row.className}\0${row.heap}`);
    row.reachableSize = s?.reachable ?? 0;
    row.reachableNativeSize = s?.reachableNative ?? 0;
    row.reachableCount = s?.reachableCount ?? 0;
  }
}

/**
 * Enrich PrimOrRef fields with reachable sizes (for ref-kind fields only).
 */
export async function enrichFieldsWithReachable(
  engine: Engine,
  fields: {name: string; typeName: string; value: PrimOrRef}[],
): Promise<void> {
  const ids: number[] = [];
  for (const f of fields) {
    if (f.value.kind === 'ref' && f.value.reachableJava === undefined) {
      ids.push(f.value.id);
    }
  }
  if (ids.length === 0) return;
  const map = await getReachableSizes(engine, ids);
  for (const f of fields) {
    if (f.value.kind === 'ref' && f.value.reachableJava === undefined) {
      const s = map.get(f.value.id);
      f.value.reachableJava = s?.size ?? 0;
      f.value.reachableNative = s?.native ?? 0;
      f.value.reachableCount = s?.count ?? 0;
    }
  }
}

/**
 * Enrich array elements with reachable sizes (for ref-kind values only).
 */
export async function enrichArrayElemsWithReachable(
  engine: Engine,
  elems: {idx: number; value: PrimOrRef}[],
): Promise<void> {
  const ids: number[] = [];
  for (const e of elems) {
    if (e.value.kind === 'ref' && e.value.reachableJava === undefined) {
      ids.push(e.value.id);
    }
  }
  if (ids.length === 0) return;
  const map = await getReachableSizes(engine, ids);
  for (const e of elems) {
    if (e.value.kind === 'ref' && e.value.reachableJava === undefined) {
      const s = map.get(e.value.id);
      e.value.reachableJava = s?.size ?? 0;
      e.value.reachableNative = s?.native ?? 0;
      e.value.reachableCount = s?.count ?? 0;
    }
  }
}
