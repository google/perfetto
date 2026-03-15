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

// SQL query layer — pure functions mapping (Engine) → Promise<DisplayType>.
// Replaces the Web Worker RPC protocol with direct trace processor queries.

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
  ClassRow,
} from './types';
import {fmtHex} from './format';

// ─── Helpers ──────────────────────────────────────────────────────────────────

function className(name: string | null, deobfuscated: string | null): string {
  return deobfuscated ?? name ?? '???';
}

function shortClassName(full: string): string {
  // "com.foo.Bar[]" → "Bar[]"
  const bracket = full.indexOf('[');
  const base = bracket >= 0 ? full.slice(0, bracket) : full;
  const dot = base.lastIndexOf('.');
  const short = dot >= 0 ? base.slice(dot + 1) : base;
  return bracket >= 0 ? short + full.slice(bracket) : short;
}

function makeDisplay(cls: string, id: number): string {
  return `${shortClassName(cls)} ${fmtHex(id)}`;
}

// Map C++ class kind strings to reachability names.
const KIND_TO_REACHABILITY: Record<string, string> = {
  KIND_WEAK_REFERENCE: 'weak',
  KIND_SOFT_REFERENCE: 'soft',
  KIND_PHANTOM_REFERENCE: 'phantom',
  KIND_FINALIZER_REFERENCE: 'finalizer',
};

/** Build a minimal InstanceRow from common SQL columns. */
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
  value_string: string | null;
  class_kind?: string;
}): InstanceRow {
  const cls = className(it.cls, it.deob);
  const retainedJava = it.dominated_size ?? it.self_size;
  const retainedNative = it.dominated_native ?? it.native_size;
  const heap = it.heap_type ?? 'default';
  // Reachability from class kind (C++ parser propagates to all subclasses).
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
    retainedByHeap: [{heap, java: retainedJava, native_: retainedNative}],
    str: it.value_string,
    referent: null,
  };
}

// The common SQL column list for building InstanceRows.
const INSTANCE_COLS = `
  o.id, c.name AS cls, c.deobfuscated_name AS deob,
  o.self_size, o.native_size, o.heap_type, o.root_type,
  d.dominated_size_bytes AS dominated_size,
  d.dominated_native_size_bytes AS dominated_native,
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

// ─── Overview ─────────────────────────────────────────────────────────────────

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

  // Duplicate bitmaps: find bitmaps with the same (width, height).
  // First query gets per-bitmap dimensions + retained size.
  const dupRes = await engine.query(`
    SELECT
      MAX(CASE WHEN f.field_name GLOB '*mWidth' THEN f.int_value END) AS w,
      MAX(CASE WHEN f.field_name GLOB '*mHeight' THEN f.int_value END) AS h,
      ifnull(d.dominated_size_bytes, o.self_size)
        + ifnull(d.dominated_native_size_bytes, o.native_size) AS total_bytes
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
    LEFT JOIN heap_graph_object_field f ON f.field_set_id = od.field_set_id
    WHERE o.reachable != 0
      AND (c.name = 'android.graphics.Bitmap'
        OR c.deobfuscated_name = 'android.graphics.Bitmap')
    GROUP BY o.id
    HAVING w IS NOT NULL AND h IS NOT NULL
  `);
  // Aggregate by (w, h) to find duplicates.
  const dimGroups = new Map<
    string,
    {w: number; h: number; cnt: number; total: number; min: number}
  >();
  for (
    const it = dupRes.iter({w: NUM, h: NUM, total_bytes: NUM});
    it.valid();
    it.next()
  ) {
    const key = `${it.w}x${it.h}`;
    const existing = dimGroups.get(key);
    if (existing) {
      existing.cnt++;
      existing.total += it.total_bytes;
      existing.min = Math.min(existing.min, it.total_bytes);
    } else {
      dimGroups.set(key, {
        w: it.w,
        h: it.h,
        cnt: 1,
        total: it.total_bytes,
        min: it.total_bytes,
      });
    }
  }
  const duplicateBitmaps: DuplicateBitmapGroup[] = [];
  for (const g of dimGroups.values()) {
    if (g.cnt < 2) continue;
    duplicateBitmaps.push({
      width: g.w,
      height: g.h,
      count: g.cnt,
      totalBytes: g.total,
      wastedBytes: g.total - g.min,
    });
  }
  duplicateBitmaps.sort((a, b) => b.wastedBytes - a.wastedBytes);

  return {
    instanceCount,
    heaps,
    duplicateBitmaps:
      duplicateBitmaps.length > 0 ? duplicateBitmaps : undefined,
  };
}

// ─── Allocations (class histogram) ───────────────────────────────────────────

export async function getAllocations(
  engine: Engine,
  heap: string | null,
): Promise<ClassRow[]> {
  const heapFilter = heap
    ? `AND o.heap_type = '${heap.replace(/'/g, "''")}'`
    : '';
  const res = await engine.query(`
    SELECT
      ifnull(c.deobfuscated_name, c.name) AS cls,
      COUNT(*) AS cnt,
      SUM(o.self_size + o.native_size) AS shallow,
      SUM(ifnull(d.dominated_size_bytes, o.self_size)
        + ifnull(d.dominated_native_size_bytes, o.native_size)) AS retained,
      ifnull(o.heap_type, 'default') AS heap
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    WHERE o.reachable != 0
      ${heapFilter}
    GROUP BY cls, heap
    ORDER BY retained DESC
  `);
  const rows: ClassRow[] = [];
  for (
    const it = res.iter({
      cls: STR,
      cnt: NUM,
      shallow: NUM,
      retained: NUM,
      heap: STR,
    });
    it.valid();
    it.next()
  ) {
    rows.push({
      className: it.cls,
      count: it.cnt,
      shallowSize: it.shallow,
      retainedSize: it.retained,
      heap: it.heap,
    });
  }
  return rows;
}

// ─── Rooted ───────────────────────────────────────────────────────────────────

export async function getRooted(engine: Engine): Promise<InstanceRow[]> {
  const res = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_dominator_tree d
    JOIN heap_graph_object o ON d.id = o.id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
    WHERE d.idom_id IS NULL
    ORDER BY d.dominated_size_bytes + d.dominated_native_size_bytes DESC
  `);
  return collectRows(res);
}

// ─── getInstance ──────────────────────────────────────────────────────────────

export async function getInstance(
  engine: Engine,
  id: number,
): Promise<InstanceDetail | null> {
  // 1. Object info
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
      od.array_element_count,
      od.array_data_id,
      d.dominated_size_bytes AS dominated_size,
      d.dominated_native_size_bytes AS dominated_native
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_class sc ON c.superclass_id = sc.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
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
    array_element_count: NUM_NULL,
    array_data_id: NUM_NULL,
    dominated_size: NUM_NULL,
    dominated_native: NUM_NULL,
  });
  if (!oit.valid()) return null;

  const fullClassName = className(oit.cls, oit.deob);
  const classKind = oit.class_kind;
  const superClassName = oit.super_cls;
  const refSetId = oit.reference_set_id;
  const fieldSetId = oit.field_set_id;
  const arrayDataId = oit.array_data_id;
  const arrayElementType = oit.array_element_type;
  const arrayElementCount = oit.array_element_count;

  // Class objects in the C++ parser are named "java.lang.Class<ClassName>".
  const isClassObj = fullClassName.startsWith('java.lang.Class<');
  const isArrayInstance = fullClassName.endsWith('[]');

  // Reachability is determined by class kind (already propagated to subclasses
  // by the C++ HPROF parser's superclass walk).
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
      LEFT JOIN heap_graph_object_data od2 ON od2.object_id = o2.id
      WHERE r.reference_set_id = ${refSetId}
        AND (r.field_name GLOB '*referent'
          OR r.deobfuscated_field_name GLOB '*referent')
      LIMIT 1
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
        retainedByHeap: [],
        str: rit.ref_str,
        referent: null,
      };
    }
  }

  // Get the class object row for display.
  // The class object is the java.lang.Class<X> instance.  The C++ HPROF
  // parser creates a class entry named "java.lang.Class<X>" for each class X,
  // using X's obfuscated name.
  let classObjRow: InstanceRow | null = null;
  {
    const classObjName = `java.lang.Class<${oit.cls}>`.replace(/'/g, "''");
    const cRes = await engine.query(`
      SELECT ${INSTANCE_COLS}
      FROM heap_graph_object o
      JOIN heap_graph_class c ON o.type_id = c.id
      LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
      LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
      WHERE c.name = '${classObjName}'
      LIMIT 1
    `);
    const rows = collectRows(cRes);
    if (rows.length > 0) classObjRow = rows[0];
  }

  // 2. Instance fields (primitives from heap_graph_object_field)
  // For class objects, fields go into staticFields instead (handled below).
  const instanceFields: InstanceDetail['instanceFields'] = [];
  if (fieldSetId !== null && !isArrayInstance && !isClassObj) {
    const fRes = await engine.query(`
      SELECT field_name, field_type,
        bool_value, byte_value, char_value, short_value,
        int_value, long_value, float_value, double_value, string_value
      FROM heap_graph_object_field
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
        string_value: STR_NULL,
      });
      fit.valid();
      fit.next()
    ) {
      const v = primFieldValue(fit);
      instanceFields.push({
        name: fit.field_name,
        typeName: fit.field_type,
        value: v,
      });
    }
  }

  // 3. Reference fields (object references from heap_graph_reference)
  if (refSetId !== null && !isArrayInstance && !isClassObj) {
    const rRes = await engine.query(`
      SELECT
        ifnull(r.deobfuscated_field_name, r.field_name) AS fname,
        r.field_type_name AS ftype,
        r.owned_id,
        c2.name AS ref_cls,
        c2.deobfuscated_name AS ref_deob,
        od2.value_string AS ref_str
      FROM heap_graph_reference r
      LEFT JOIN heap_graph_object o2 ON r.owned_id = o2.id
      LEFT JOIN heap_graph_class c2 ON o2.type_id = c2.id
      LEFT JOIN heap_graph_object_data od2 ON od2.object_id = o2.id
      WHERE r.reference_set_id = ${refSetId}
      ORDER BY fname
    `);
    for (
      const rit = rRes.iter({
        fname: STR,
        ftype: STR,
        owned_id: NUM_NULL,
        ref_cls: STR_NULL,
        ref_deob: STR_NULL,
        ref_str: STR_NULL,
      });
      rit.valid();
      rit.next()
    ) {
      if (rit.owned_id === null || rit.owned_id === 0) {
        instanceFields.push({
          name: rit.fname,
          typeName: rit.ftype,
          value: {kind: 'prim', v: 'null'},
        });
      } else {
        const refCls = className(rit.ref_cls, rit.ref_deob);
        instanceFields.push({
          name: rit.fname,
          typeName: rit.ftype,
          value: {
            kind: 'ref',
            id: rit.owned_id,
            display: makeDisplay(refCls, rit.owned_id),
            str: rit.ref_str,
          },
        });
      }
    }
  }

  // 4. Array elements
  let arrayLength = 0;
  let elemTypeName: string | null = null;
  const arrayElems: InstanceDetail['arrayElems'] = [];
  if (isArrayInstance) {
    elemTypeName = fullClassName.slice(0, -2); // "int[]" → "int"
    if (arrayDataId !== null && arrayElementType !== null) {
      // Primitive array: decode from BLOB via SQL function.
      const blobRes = await engine.query(`
        SELECT __intrinsic_heap_graph_get_array(${arrayDataId}) AS data
      `);
      const bit = blobRes.iter({data: BLOB_NULL});
      if (bit.valid() && bit.data !== null) {
        const elems = decodePrimitiveArray(
          arrayElementType,
          arrayElementCount ?? 0,
          bit.data,
        );
        arrayElems.push(...elems);
        arrayLength = arrayElementCount ?? 0;
      }
    } else if (refSetId !== null) {
      // Object array elements (String[], Object[], etc.)
      const oaRes = await engine.query(`
        SELECT
          r.field_name AS fname,
          r.owned_id,
          c2.name AS ref_cls,
          c2.deobfuscated_name AS ref_deob,
          od2.value_string AS ref_str
        FROM heap_graph_reference r
        LEFT JOIN heap_graph_object o2 ON r.owned_id = o2.id
        LEFT JOIN heap_graph_object_data od2 ON od2.object_id = o2.id
        LEFT JOIN heap_graph_class c2 ON o2.type_id = c2.id
        WHERE r.reference_set_id = ${refSetId}
        ORDER BY CAST(SUBSTR(r.field_name, 2,
          LENGTH(r.field_name) - 2) AS INTEGER)
        LIMIT 10000
      `);
      for (
        const oait = oaRes.iter({
          fname: STR,
          owned_id: NUM_NULL,
          ref_cls: STR_NULL,
          ref_deob: STR_NULL,
          ref_str: STR_NULL,
        });
        oait.valid();
        oait.next()
      ) {
        const idx = parseInt(oait.fname.slice(1, oait.fname.length - 1), 10);
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
          };
        }
        arrayElems.push({idx, value});
        arrayLength = Math.max(arrayLength, idx + 1);
      }
    }
  }

  // 5. Reverse references
  const revRes = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_reference r
    JOIN heap_graph_object o ON r.owner_id = o.id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
    WHERE r.owned_id = ${id}
    ORDER BY (ifnull(d.dominated_size_bytes, 0)
      + ifnull(d.dominated_native_size_bytes, 0)) DESC
    LIMIT 200
  `);
  const reverseRefs = collectRows(revRes);

  // 6. Dominated objects
  const domRes = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_dominator_tree d
    JOIN heap_graph_object o ON d.id = o.id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
    WHERE d.idom_id = ${id}
    ORDER BY (d.dominated_size_bytes + d.dominated_native_size_bytes) DESC
    LIMIT 200
  `);
  const dominated = collectRows(domRes);

  // 7. Path from root (walk dominator tree upward, with connecting field names)
  const pathFromRoot: InstanceDetail['pathFromRoot'] = [];
  {
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
        od.value_string, c.kind AS class_kind
      FROM path p
      JOIN heap_graph_object o ON o.id = p.obj_id
      JOIN heap_graph_class c ON o.type_id = c.id
      LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
      LEFT JOIN heap_graph_dominator_tree dt ON dt.id = o.id
      ORDER BY p.depth DESC
    `);
    // Collect path entries (root first, target last).
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
        value_string: STR_NULL,
        class_kind: STR,
      });
      pit.valid();
      pit.next()
    ) {
      entries.push({row: rowFromIter(pit), objId: pit.id});
    }

    // For each consecutive pair, find the field name connecting them.
    for (let i = 0; i < entries.length; i++) {
      let field = '';
      if (i < entries.length - 1) {
        const parentId = entries[i].objId;
        const childId = entries[i + 1].objId;
        // Look for a reference from parent to child.
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
      pathFromRoot.push({
        row: entries[i].row,
        field,
        isDominator: true,
      });
    }
  }

  // 8. Static fields (for class objects — query their own fields)
  const staticFields: InstanceDetail['staticFields'] = [];
  if (isClassObj && fieldSetId !== null) {
    // Primitive static fields
    const sfRes = await engine.query(`
      SELECT field_name, field_type,
        bool_value, byte_value, char_value, short_value,
        int_value, long_value, float_value, double_value, string_value
      FROM heap_graph_object_field
      WHERE field_set_id = ${fieldSetId}
      ORDER BY field_name
    `);
    for (
      const sfit = sfRes.iter({
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
        string_value: STR_NULL,
      });
      sfit.valid();
      sfit.next()
    ) {
      staticFields.push({
        name: sfit.field_name,
        typeName: sfit.field_type,
        value: primFieldValue(sfit),
      });
    }
    // Reference static fields
    if (refSetId !== null) {
      const srRes = await engine.query(`
        SELECT
          ifnull(r.deobfuscated_field_name, r.field_name) AS fname,
          r.field_type_name AS ftype,
          r.owned_id,
          c2.name AS ref_cls,
          c2.deobfuscated_name AS ref_deob,
          od2.value_string AS ref_str
        FROM heap_graph_reference r
        LEFT JOIN heap_graph_object o2 ON r.owned_id = o2.id
        LEFT JOIN heap_graph_object_data od2 ON od2.object_id = o2.id
        LEFT JOIN heap_graph_class c2 ON o2.type_id = c2.id
        WHERE r.reference_set_id = ${refSetId}
        ORDER BY fname
      `);
      for (
        const srit = srRes.iter({
          fname: STR,
          ftype: STR,
          owned_id: NUM_NULL,
          ref_cls: STR_NULL,
          ref_deob: STR_NULL,
          ref_str: STR_NULL,
        });
        srit.valid();
        srit.next()
      ) {
        if (srit.owned_id === null || srit.owned_id === 0) {
          staticFields.push({
            name: srit.fname,
            typeName: srit.ftype,
            value: {kind: 'prim', v: 'null'},
          });
        } else {
          const refCls = className(srit.ref_cls, srit.ref_deob);
          staticFields.push({
            name: srit.fname,
            typeName: srit.ftype,
            value: {
              kind: 'ref',
              id: srit.owned_id,
              display: makeDisplay(refCls, srit.owned_id),
              str: srit.ref_str,
            },
          });
        }
      }
    }
  }

  // 9. Bitmap pixel data via DumpData (modern Android API 26+).
  let bitmap: InstanceDetail['bitmap'] = null;
  if (fullClassName === 'android.graphics.Bitmap') {
    bitmap = await extractBitmapPixels(engine, fieldSetId);
  }
  // Also check for deobfuscated Bitmap class name.
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
      WHERE c.name = '${superObjName}'
      LIMIT 1
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
    pathFromRoot: pathFromRoot.length > 0 ? pathFromRoot : null,
  };
}

// ─── Download helpers ─────────────────────────────────────────────────────────

export async function getRawArrayBlob(
  engine: Engine,
  objectId: number,
): Promise<Uint8Array | null> {
  const res = await engine.query(`
    SELECT __intrinsic_heap_graph_get_array(od.array_data_id) AS data
    FROM heap_graph_object_data od
    WHERE od.object_id = ${objectId} AND od.array_data_id IS NOT NULL
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
    FROM heap_graph_object_data od
    JOIN heap_graph_object_field f ON f.field_set_id = od.field_set_id
    WHERE od.object_id = ${objectId}
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
    SELECT __intrinsic_heap_graph_get_array(od.array_data_id) AS data
    FROM heap_graph_object_data od WHERE od.object_id = ${bufferObjId}
  `);
  const bit = bufRes.iter({data: BLOB_NULL});
  if (!bit.valid() || bit.data === null) return null;

  const format = DUMP_DATA_FORMAT_NAMES[dumpData.format] ?? 'png';
  return {data: bit.data, format};
}

// ─── Primitive array blob decoding ────────────────────────────────────────────

const DISPLAY_LIMIT = 10000;

function decodePrimitiveArray(
  type: string,
  count: number,
  blob: Uint8Array,
): {idx: number; value: PrimOrRef}[] {
  const dv = new DataView(blob.buffer, blob.byteOffset, blob.byteLength);
  const limit = Math.min(count, DISPLAY_LIMIT);
  const elems: {idx: number; value: PrimOrRef}[] = [];
  for (let i = 0; i < limit; i++) {
    let v: string;
    switch (type) {
      case 'boolean':
        v = blob[i] ? 'true' : 'false';
        break;
      case 'byte':
        v = String(dv.getInt8(i));
        break;
      case 'char': {
        const c = dv.getUint16(i * 2, true);
        v = c >= 32 && c < 127 ? `'${String.fromCharCode(c)}'` : String(c);
        break;
      }
      case 'short':
        v = String(dv.getInt16(i * 2, true));
        break;
      case 'int':
        v = String(dv.getInt32(i * 4, true));
        break;
      case 'long':
        v = String(dv.getBigInt64(i * 8, true));
        break;
      case 'float':
        v = String(dv.getFloat32(i * 4, true));
        break;
      case 'double':
        v = String(dv.getFloat64(i * 8, true));
        break;
      default:
        v = '???';
    }
    elems.push({idx: i, value: {kind: 'prim', v}});
  }
  return elems;
}

// ─── Bitmap DumpData extraction ──────────────────────────────────────────────
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
    WHERE c.name LIKE '%Class<android.graphics.Bitmap>'
    LIMIT 1
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
    LIMIT 1
  `);
  const ddIt = ddRes.iter({dump_data_id: NUM_NULL});
  if (!ddIt.valid() || ddIt.dump_data_id === null) {
    cachedDumpData = null;
    return null;
  }
  const dumpDataId = ddIt.dump_data_id;

  // Step 3: Read format from DumpData's fields.
  const fmtRes = await engine.query(`
    SELECT int_value FROM heap_graph_object_field
    WHERE field_set_id = (SELECT field_set_id FROM heap_graph_object_data WHERE object_id = ${dumpDataId})
      AND field_name GLOB '*format'
    LIMIT 1
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

  // Step 5: Decode natives long[] as BigInt64Array.
  const nativesRes = await engine.query(`
    SELECT __intrinsic_heap_graph_get_array(od.array_data_id) AS data
    FROM heap_graph_object_data od WHERE od.object_id = ${nativesObjId}
  `);
  const nativesIt = nativesRes.iter({data: BLOB_NULL});
  if (!nativesIt.valid() || nativesIt.data === null) {
    cachedDumpData = null;
    return null;
  }
  const nativesBlob = nativesIt.data;
  const nativesPtrs = new BigInt64Array(
    nativesBlob.buffer,
    nativesBlob.byteOffset,
    nativesBlob.byteLength / 8,
  );

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

  // Read mWidth, mHeight, mNativePtr from instance fields.
  const dimRes = await engine.query(`
    SELECT field_name, int_value, long_value
    FROM heap_graph_object_field
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

  // Look up compressed buffer via DumpData.
  const dumpData = await loadBitmapDumpData(engine);
  if (dumpData === null) return null;

  const bufferObjId = dumpData.bufferMap.get(nativePtr);
  if (bufferObjId === undefined) return null;

  // Fetch the compressed image bytes.
  const bufRes = await engine.query(`
    SELECT __intrinsic_heap_graph_get_array(od.array_data_id) AS data
    FROM heap_graph_object_data od WHERE od.object_id = ${bufferObjId}
  `);
  const bufIt = bufRes.iter({data: BLOB_NULL});
  if (!bufIt.valid() || bufIt.data === null) return null;

  const format = DUMP_DATA_FORMAT_NAMES[dumpData.format] ?? 'png';
  return {width, height, format, data: bufIt.data};
}

/** Load bitmap pixel data for a single object by ID. */
export async function getBitmapPixels(
  engine: Engine,
  objectId: number,
): Promise<InstanceDetail['bitmap']> {
  const res = await engine.query(`
    SELECT od.field_set_id
    FROM heap_graph_object_data od
    WHERE od.object_id = ${objectId}
    LIMIT 1
  `);
  const row = res.maybeFirstRow({field_set_id: NUM_NULL});
  return extractBitmapPixels(engine, row?.field_set_id ?? null);
}

// ─── Search ───────────────────────────────────────────────────────────────────

export async function search(
  engine: Engine,
  query: string,
): Promise<InstanceRow[]> {
  // Support hex ID search (e.g. "0x12ab34cd")
  if (query.startsWith('0x') || query.startsWith('0X')) {
    const numId = parseInt(query, 16);
    if (!isNaN(numId)) {
      const res = await engine.query(`
        SELECT ${INSTANCE_COLS}
        FROM heap_graph_object o
        JOIN heap_graph_class c ON o.type_id = c.id
        LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
        LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
        WHERE o.id = ${numId}
      `);
      return collectRows(res);
    }
  }

  // Escape SQL LIKE wildcards in user input
  const escaped = query.replace(/[%_\\]/g, '\\$&').replace(/'/g, "''");
  const res = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
    WHERE o.reachable != 0
      AND (c.name LIKE '%${escaped}%' ESCAPE '\\'
        OR c.deobfuscated_name LIKE '%${escaped}%' ESCAPE '\\')
    ORDER BY (ifnull(d.dominated_size_bytes, 0)
      + ifnull(d.dominated_native_size_bytes, 0)) DESC
    LIMIT 1000
  `);
  return collectRows(res);
}

// ─── getObjects ───────────────────────────────────────────────────────────────

export async function getObjects(
  engine: Engine,
  cls: string,
  heap: string | null,
): Promise<InstanceRow[]> {
  const escaped = cls.replace(/'/g, "''");
  const heapFilter = heap
    ? `AND o.heap_type = '${heap.replace(/'/g, "''")}'`
    : '';
  const res = await engine.query(`
    SELECT ${INSTANCE_COLS}
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
    WHERE o.reachable != 0
      AND (c.name = '${escaped}' OR c.deobfuscated_name = '${escaped}')
      ${heapFilter}
    ORDER BY o.self_size + o.native_size DESC
    LIMIT 5000
  `);
  return collectRows(res);
}

// ─── getObjectsByFlamegraphSelection ──────────────────────────────────────────

export async function getObjectsByFlamegraphSelection(
  engine: Engine,
  pathHashes: string,
  isDominator: boolean,
): Promise<InstanceRow[]> {
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
    WITH _ahat_sel(path_hash) AS (VALUES ${values})
    SELECT ${INSTANCE_COLS}
    FROM _ahat_sel f
    JOIN ${hashTable} h ON h.path_hash = f.path_hash
    JOIN heap_graph_object o ON o.id = h.id
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
    ORDER BY o.self_size + o.native_size DESC
    LIMIT 5000
  `);
  return collectRows(res);
}

// ─── getHeapGraphTrackInfo ─────────────────────────────────────────────────────

export async function getHeapGraphTrackInfo(
  engine: Engine,
  objectId: number,
): Promise<{
  upid: number;
  eventId: number;
  className: string | null;
  pathHash: string | null;
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

  // Look up the object's path hash in both class tree and dominator tree.
  // The dominator tree is always materialized by the Ahat plugin; the class
  // tree tables exist if the HeapProfile plugin rendered a flamegraph.
  let pathHash: string | null = null;
  for (const table of [
    '_heap_graph_dominator_path_hashes',
    '_heap_graph_path_hashes',
  ]) {
    try {
      const ph = await engine.query(`
        SELECT CAST(path_hash AS TEXT) AS ph
        FROM ${table} WHERE id = ${objectId} LIMIT 1
      `);
      const phRow = ph.maybeFirstRow({ph: STR_NULL});
      if (phRow?.ph) {
        pathHash = phRow.ph;
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
  };
}

// ─── getStringList ────────────────────────────────────────────────────────────

export async function getStringList(engine: Engine): Promise<StringListRow[]> {
  const res = await engine.query(`
    SELECT
      o.id,
      od.value_string AS value,
      o.self_size,
      o.heap_type,
      c.name AS cls,
      c.deobfuscated_name AS deob,
      d.dominated_size_bytes AS dominated_size,
      d.dominated_native_size_bytes AS dominated_native
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
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
    const retained =
      (it.dominated_size ?? it.self_size) + (it.dominated_native ?? 0);
    rows.push({
      id: it.id,
      value: it.value,
      length: it.value.length,
      retainedSize: retained,
      shallowSize: it.self_size,
      heap: it.heap_type ?? 'default',
      className: fullCls,
      display: makeDisplay(fullCls, it.id),
    });
  }
  return rows;
}

// ─── getBitmapList ────────────────────────────────────────────────────────────

export async function getBitmapList(engine: Engine): Promise<BitmapListRow[]> {
  // Load DumpData once to check which bitmaps have pixel data.
  const dumpData = await loadBitmapDumpData(engine);

  // Get bitmap instances with mWidth/mHeight/mNativePtr from object_field table.
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
      od.value_string,
      c.kind AS class_kind,
      MAX(CASE WHEN f.field_name GLOB '*mWidth' THEN f.int_value END) AS width,
      MAX(CASE WHEN f.field_name GLOB '*mHeight' THEN f.int_value END) AS height,
      MAX(CASE WHEN f.field_name GLOB '*mDensity' THEN f.int_value END) AS density,
      MAX(CASE WHEN f.field_name GLOB '*mNativePtr' THEN f.long_value END) AS native_ptr
    FROM heap_graph_object o
    JOIN heap_graph_class c ON o.type_id = c.id
    LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
    LEFT JOIN heap_graph_dominator_tree d ON d.id = o.id
    LEFT JOIN heap_graph_object_field f ON f.field_set_id = od.field_set_id
    WHERE o.reachable != 0
      AND (c.name = 'android.graphics.Bitmap'
        OR c.deobfuscated_name = 'android.graphics.Bitmap')
    GROUP BY o.id
    ORDER BY (ifnull(d.dominated_size_bytes, 0)
      + ifnull(d.dominated_native_size_bytes, 0)) DESC
  `);

  const rows: BitmapListRow[] = [];
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
    // Check if DumpData has a buffer for this bitmap's native pointer.
    let hasPixelData = false;
    if (dumpData !== null && it.native_ptr !== null && w > 0 && h > 0) {
      hasPixelData = dumpData.bufferMap.has(it.native_ptr);
    }
    rows.push({
      row: rowFromIter(it),
      width: w,
      height: h,
      pixelCount: w * h,
      bufferHash: '',
      hasPixelData,
      density: it.density ?? 0,
    });
  }
  return rows;
}

// ─── Primitive field value extraction ─────────────────────────────────────────

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
  string_value: string | null;
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
      return {kind: 'prim', v: it.string_value ?? '???'};
  }
}
