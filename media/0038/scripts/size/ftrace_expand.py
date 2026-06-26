#!/usr/bin/env python3
"""ftrace de-bundling analyzer (tracing-v2 Task 01).

Measures the SMB bandwidth amplification of rewriting ftrace as "one TracePacket
per ftrace event" (no bundling, no CompactSched columnar packing) — the v2
"producer firehose" model. Self-serve: run it on any trace and paste the emitted
JSON back for cross-trace analysis.

  See tracing_v2/tasks/01-ftrace-debundling-*.md.

WHAT IT DOES
  - keeps only ftrace packets;
  - expands each FtraceEventBundle into one single-event TracePacket per event
    (cpu moved onto the FtraceEvent; the bundle is killed, no wrapper);
  - reconstructs each CompactSched row into a full sched_switch/sched_waking
    event (un-delta timestamp, un-intern comm, prev_* rebuilt by per-cpu chain);
  - reports amplification (expanded/baseline) at payload and file level, broken
    down by class, plus a per-event-type histogram.

INPUT MUST BE DECOMPRESSED. If your trace was recorded with compression, run:
    out/linux_clang_release/traceconv decompress_packets in.pftrace decomp.pftrace
(the tool errors out if it sees compressed_packets).

USAGE
    python3 tools/ftrace_expand.py decomp.pftrace --label "first_unlock/t1"
    # add --json to print only the JSON blob (for paste-back)
    # add --write-traces to also emit ftrace_only.pftrace / ftrace_expanded.pftrace
    #   (needed only for Task 03 recompression)
"""

import argparse
import json
import os
import sys

# --- field numbers (see the .proto files) ----------------------------------
F_TRACE_PACKET = 1
F_TP_FTRACE_EVENTS = 1
F_TP_SEQ_ID = 10
F_TP_COMPRESSED = 50
F_TP_FTRACE_EVENT = 1   # v2 provisional single-event field (see analysis doc)

F_FEB_CPU = 1
F_FEB_EVENT = 2
F_FEB_LOST_EVENTS = 3
F_FEB_COMPACT_SCHED = 4

F_FE_TIMESTAMP = 1
F_FE_PID = 2
F_FE_COMMON_FLAGS = 5
F_FE_CPU = 6            # NEW (provisional): cpu moved off the bundle
F_FE_SCHED_SWITCH = 4
F_FE_SCHED_WAKING = 20

F_CS_SWITCH_TIMESTAMP = 1
F_CS_SWITCH_PREV_STATE = 2
F_CS_SWITCH_NEXT_PID = 3
F_CS_SWITCH_NEXT_PRIO = 4
F_CS_INTERN_TABLE = 5
F_CS_SWITCH_NEXT_COMM_INDEX = 6
F_CS_WAKING_TIMESTAMP = 7
F_CS_WAKING_PID = 8
F_CS_WAKING_TARGET_CPU = 9
F_CS_WAKING_PRIO = 10
F_CS_WAKING_COMM_INDEX = 11
F_CS_WAKING_COMMON_FLAGS = 12

F_SS_PREV_COMM, F_SS_PREV_PID, F_SS_PREV_PRIO, F_SS_PREV_STATE = 1, 2, 3, 4
F_SS_NEXT_COMM, F_SS_NEXT_PID, F_SS_NEXT_PRIO = 5, 6, 7
F_SW_COMM, F_SW_PID, F_SW_PRIO, F_SW_SUCCESS, F_SW_TARGET_CPU = 1, 2, 3, 4, 5

# --- wire helpers (hand-rolled; mirrors tools/wakelock_extract.py) ----------


def encode_varint(value):
  if value < 0:
    value += 1 << 64
  out = bytearray()
  while value > 0x7F:
    out.append(0x80 | (value & 0x7F))
    value >>= 7
  out.append(value & 0x7F)
  return bytes(out)


def decode_varint(data, pos):
  result = shift = 0
  while True:
    b = data[pos]
    result |= (b & 0x7F) << shift
    pos += 1
    if not (b & 0x80):
      return result, pos
    shift += 7


def tag(field_num, wire_type):
  return encode_varint((field_num << 3) | wire_type)


def field_varint(field_num, value):
  return tag(field_num, 0) + encode_varint(value)


def field_bytes(field_num, data):
  return tag(field_num, 2) + encode_varint(len(data)) + data


def field_string(field_num, s):
  return field_bytes(field_num, s.encode('utf-8') if isinstance(s, str) else s)


def iter_fields(data):
  pos, n = 0, len(data)
  while pos < n:
    key, pos = decode_varint(data, pos)
    fnum, wt = key >> 3, key & 0x7
    if wt == 0:
      val, pos = decode_varint(data, pos)
      yield fnum, wt, val
    elif wt == 2:
      ln, pos = decode_varint(data, pos)
      yield fnum, wt, data[pos:pos + ln]
      pos += ln
    elif wt == 1:
      yield fnum, wt, data[pos:pos + 8]
      pos += 8
    elif wt == 5:
      yield fnum, wt, data[pos:pos + 4]
      pos += 4
    else:
      raise ValueError(f'bad wire type {wt} at {pos}')


def decode_packed_varints(blob):
  out, pos, n = [], 0, len(blob)
  while pos < n:
    v, pos = decode_varint(blob, pos)
    out.append(v)
  return out


def to_signed64(v):
  return v - (1 << 64) if v >= (1 << 63) else v


# --- FtraceEvent oneof name map (parsed from proto if available) ------------


def load_event_names(explicit_path=None):
  """field_number -> event name, parsed from ftrace_event.proto's `oneof event`.
  Returns {} if the proto can't be found (histogram falls back to field_N)."""
  rel = 'protos/perfetto/trace/ftrace/ftrace_event.proto'
  candidates = []
  if explicit_path:
    candidates.append(explicit_path)
  here = os.path.dirname(os.path.abspath(__file__))
  for base in (here, os.getcwd()):
    d = base
    for _ in range(6):  # walk up looking for the repo root
      candidates.append(os.path.join(d, rel))
      d = os.path.dirname(d)
  for path in candidates:
    if path and os.path.isfile(path):
      names = {}
      in_oneof = False
      import re
      for line in open(path):
        if 'oneof event {' in line:
          in_oneof = True
          continue
        if in_oneof:
          if line.strip() == '}':
            break
          m = re.match(r'\s*\w+\s+(\w+)\s*=\s*(\d+);', line)
          if m:
            names[int(m.group(2))] = m.group(1)
      if names:
        return names
  return {}


# --- expansion --------------------------------------------------------------


class Stats:

  def __init__(self):
    self.bundles = 0
    self.individual_events = 0
    self.compact_switch = 0
    self.compact_waking = 0
    self.lost_events_bundles = 0
    self.baseline_payload = 0
    self.baseline_payload_individual = 0
    self.baseline_payload_compact = 0
    self.expanded_payload = 0
    self.expanded_payload_individual = 0
    self.expanded_payload_compact = 0
    self.baseline_file = 0
    self.expanded_file = 0
    # with-TracePacket-framing (file) bytes per class/type, for the
    # "incl. per-event packet header" scenario metric
    self.expanded_file_individual = 0
    self.expanded_file_compact = 0
    self.indiv_file_by_type = {}
    # per-type: type_key -> [count, expanded_bytes]
    self.by_type = {}
    # per individual-event type: type_key -> baseline_bytes (for scenario maths)
    self.indiv_baseline_by_type = {}

  @property
  def total_events(self):
    return self.individual_events + self.compact_switch + self.compact_waking

  def bump_type(self, key, exp_bytes):
    e = self.by_type.get(key)
    if e is None:
      self.by_type[key] = [1, exp_bytes]
    else:
      e[0] += 1
      e[1] += exp_bytes


def make_packet(fe_bytes, seq_id):
  body = field_bytes(F_TP_FTRACE_EVENT, fe_bytes)
  if seq_id is not None:
    body += field_varint(F_TP_SEQ_ID, seq_id)
  return field_bytes(F_TRACE_PACKET, body)


def event_type_field(ev_bytes):
  """The FtraceEvent oneof type = first wire-type-2 field (scalars are 1/2/5)."""
  for fnum, wt, _ in iter_fields(ev_bytes):
    if wt == 2:
      return fnum
  return 0


def reconstruct_compact_sched(cs_bytes, cpu, prev_chain, stats, add_cpu=True):
  cols, intern_table = {}, []
  for fnum, wt, val in iter_fields(cs_bytes):
    if fnum == F_CS_INTERN_TABLE:
      intern_table.append(val.decode('utf-8', 'replace'))
    elif wt == 2:
      cols[fnum] = decode_packed_varints(val)
    elif wt == 0:
      cols.setdefault(fnum, []).append(val)

  sw_ts = cols.get(F_CS_SWITCH_TIMESTAMP, [])
  sw_state = cols.get(F_CS_SWITCH_PREV_STATE, [])
  sw_npid = cols.get(F_CS_SWITCH_NEXT_PID, [])
  sw_nprio = cols.get(F_CS_SWITCH_NEXT_PRIO, [])
  sw_ncomm = cols.get(F_CS_SWITCH_NEXT_COMM_INDEX, [])
  ts = 0
  for i in range(len(sw_ts)):
    ts += sw_ts[i]
    npid = to_signed64(sw_npid[i]) if i < len(sw_npid) else 0
    nprio = to_signed64(sw_nprio[i]) if i < len(sw_nprio) else 0
    ncomm = intern_table[sw_ncomm[i]] if i < len(sw_ncomm) and sw_ncomm[i] < len(
        intern_table) else ''
    pstate = to_signed64(sw_state[i]) if i < len(sw_state) else 0
    pcomm, ppid, pprio = prev_chain.get(cpu, ('', 0, 0))
    ss = (field_string(F_SS_PREV_COMM, pcomm) + field_varint(F_SS_PREV_PID, ppid)
          + field_varint(F_SS_PREV_PRIO, pprio) +
          field_varint(F_SS_PREV_STATE, pstate) +
          field_string(F_SS_NEXT_COMM, ncomm) + field_varint(F_SS_NEXT_PID, npid)
          + field_varint(F_SS_NEXT_PRIO, nprio))
    fe = field_varint(F_FE_TIMESTAMP, ts) + field_varint(F_FE_PID, ppid)
    if add_cpu:
      fe += field_varint(F_FE_CPU, cpu)
    fe += field_bytes(F_FE_SCHED_SWITCH, ss)
    prev_chain[cpu] = (ncomm, npid, nprio)
    stats.compact_switch += 1
    stats.bump_type('sched_switch', len(fe))
    yield fe

  wk_ts = cols.get(F_CS_WAKING_TIMESTAMP, [])
  wk_pid = cols.get(F_CS_WAKING_PID, [])
  wk_tcpu = cols.get(F_CS_WAKING_TARGET_CPU, [])
  wk_prio = cols.get(F_CS_WAKING_PRIO, [])
  wk_comm = cols.get(F_CS_WAKING_COMM_INDEX, [])
  wk_flags = cols.get(F_CS_WAKING_COMMON_FLAGS, [])
  ts = 0
  for i in range(len(wk_ts)):
    ts += wk_ts[i]
    pid = to_signed64(wk_pid[i]) if i < len(wk_pid) else 0
    tcpu = to_signed64(wk_tcpu[i]) if i < len(wk_tcpu) else 0
    prio = to_signed64(wk_prio[i]) if i < len(wk_prio) else 0
    comm = intern_table[wk_comm[i]] if i < len(wk_comm) and wk_comm[i] < len(
        intern_table) else ''
    flags = wk_flags[i] if i < len(wk_flags) else 0
    sw = (field_string(F_SW_COMM, comm) + field_varint(F_SW_PID, pid) +
          field_varint(F_SW_PRIO, prio) + field_varint(F_SW_SUCCESS, 1) +
          field_varint(F_SW_TARGET_CPU, tcpu))
    fe = field_varint(F_FE_TIMESTAMP, ts)
    if add_cpu:
      fe += field_varint(F_FE_CPU, cpu)
    if flags:
      fe += field_varint(F_FE_COMMON_FLAGS, flags)
    fe += field_bytes(F_FE_SCHED_WAKING, sw)
    stats.compact_waking += 1
    stats.bump_type('sched_waking', len(fe))
    yield fe


def analyze(input_path, write_traces, out_dir):
  data = open(input_path, 'rb').read()
  st = Stats()
  prev_chain = {}
  baseline_out = bytearray() if write_traces else None
  expanded_out = bytearray() if write_traces else None

  for fnum, wt, packet in iter_fields(data):
    if fnum != F_TRACE_PACKET or wt != 2:
      continue
    bundle = seq_id = None
    for pf, pwt, pval in iter_fields(packet):
      if pf == F_TP_FTRACE_EVENTS and pwt == 2:
        bundle = pval
      elif pf == F_TP_SEQ_ID and pwt == 0:
        seq_id = pval
      elif pf == F_TP_COMPRESSED:
        sys.exit('ERROR: trace has compressed_packets; run '
                 '`traceconv decompress_packets <in> <out>` first.')
    if bundle is None:
      continue

    st.bundles += 1
    st.baseline_payload += len(bundle)
    base_pkt = field_bytes(F_TRACE_PACKET, packet)
    st.baseline_file += len(base_pkt)
    if baseline_out is not None:
      baseline_out += base_pkt

    cpu, individual_events, compact_sched = 0, [], None
    had_lost = False
    for bf, bwt, bval in iter_fields(bundle):
      if bf == F_FEB_CPU and bwt == 0:
        cpu = bval
      elif bf == F_FEB_EVENT and bwt == 2:
        individual_events.append(bval)
      elif bf == F_FEB_COMPACT_SCHED and bwt == 2:
        compact_sched = bval
      elif bf == F_FEB_LOST_EVENTS and bwt == 0 and bval:
        had_lost = True
    if had_lost:
      st.lost_events_bundles += 1

    for ev in individual_events:
      st.baseline_payload_individual += len(ev)
      tf = event_type_field(ev)
      st.indiv_baseline_by_type[tf] = st.indiv_baseline_by_type.get(tf, 0) + len(ev)
      fe = ev + field_varint(F_FE_CPU, cpu)
      st.individual_events += 1
      st.expanded_payload += len(fe)
      st.expanded_payload_individual += len(fe)
      st.bump_type(tf, len(fe))
      pkt = make_packet(fe, seq_id)
      st.expanded_file += len(pkt)
      st.expanded_file_individual += len(pkt)
      st.indiv_file_by_type[tf] = st.indiv_file_by_type.get(tf, 0) + len(pkt)
      if expanded_out is not None:
        expanded_out += pkt

    if compact_sched is not None:
      st.baseline_payload_compact += len(compact_sched)
      for fe in reconstruct_compact_sched(compact_sched, cpu, prev_chain, st):
        st.expanded_payload += len(fe)
        st.expanded_payload_compact += len(fe)
        pkt = make_packet(fe, seq_id)
        st.expanded_file += len(pkt)
        st.expanded_file_compact += len(pkt)
        if expanded_out is not None:
          expanded_out += pkt

  if write_traces:
    open(os.path.join(out_dir, 'ftrace_only.pftrace'), 'wb').write(baseline_out)
    open(os.path.join(out_dir, 'ftrace_expanded.pftrace'),
         'wb').write(expanded_out)
  return st


# --- reporting --------------------------------------------------------------


def ratio(a, b):
  return round(a / b, 3) if b else None


def build_result(st, label, names, top_n):
  hist = []
  for key, (cnt, eb) in sorted(st.by_type.items(), key=lambda kv: -kv[1][1]):
    if isinstance(key, str):
      name = key
    else:
      name = names.get(key, f'field_{key}')
    hist.append({
        'name': name,
        'count': cnt,
        'expanded_bytes': eb,
        'pct_expanded': round(100.0 * eb / st.expanded_payload, 2)
        if st.expanded_payload else 0.0,
    })

  # --- growth scenarios (data-only / payload level) ---
  # print = FtraceEvent field 3 (userspace atrace via trace_marker) -> moves to
  # the TrackEvent SDK in v2, so it leaves the ftrace stream entirely.
  PRINT = 3
  print_base = st.indiv_baseline_by_type.get(PRINT, 0)         # today event-data
  print_exp = st.by_type.get(PRINT, [0, 0])[1]                 # de-bundled event-data
  print_exp_file = st.indiv_file_by_type.get(PRINT, 0)         # de-bundled + pkt header
  ind_base, ind_exp = st.baseline_payload_individual, st.expanded_payload_individual
  ind_exp_file = st.expanded_file_individual
  comp_base, comp_exp = st.baseline_payload_compact, st.expanded_payload_compact
  comp_exp_file = st.expanded_file_compact
  all_base = ind_base + comp_base

  # Each scenario reports BOTH metrics, same denominator (today's event-data bytes):
  #   growth_data_only       = de-bundled EVENT bytes / today event bytes
  #   growth_with_pkt_framing= de-bundled bytes INCL. its own TracePacket header
  # (today's per-bundle header is shared/negligible, so it's folded into the
  #  denominator; the gap between the two = the per-event packet-header cost.)
  def scen(base, exp_data, exp_file):
    return {
        'today_MB': round(mb(base), 2),
        'debundled_data_only_MB': round(mb(exp_data), 2),
        'debundled_with_pkt_MB': round(mb(exp_file), 2),
        'growth_data_only': round(exp_data / base, 3) if base else None,
        'growth_with_pkt_framing': round(exp_file / base, 3) if base else None,
        'pct_of_all_bytes': round(100.0 * base / all_base, 1) if all_base else 0.0,
    }

  scenarios = {
      # what the SMB carries today, de-bundled, for various "what remains" cuts:
      'all_ftrace': scen(all_base, ind_exp + comp_exp, ind_exp_file + comp_exp_file),
      'excl_print': scen(all_base - print_base, ind_exp + comp_exp - print_exp,
                         ind_exp_file + comp_exp_file - print_exp_file),
      'excl_print_and_sched': scen(ind_base - print_base, ind_exp - print_exp,
                                   ind_exp_file - print_exp_file),
  }
  return {
      'label': label,
      'trace_processor_ftrace_event_count': None,  # fill from the TP query
      'bundles': st.bundles,
      'lost_events_bundles': st.lost_events_bundles,
      'events': {
          'total': st.total_events,
          'individual': st.individual_events,
          'compact_switch': st.compact_switch,
          'compact_waking': st.compact_waking,
      },
      'bytes': {
          'baseline_payload': st.baseline_payload,
          'expanded_payload': st.expanded_payload,
          'baseline_file': st.baseline_file,
          'expanded_file': st.expanded_file,
          'baseline_individual': st.baseline_payload_individual,
          'expanded_individual': st.expanded_payload_individual,
          'baseline_compact': st.baseline_payload_compact,
          'expanded_compact': st.expanded_payload_compact,
      },
      'amplification': {
          'payload': ratio(st.expanded_payload, st.baseline_payload),
          'file': ratio(st.expanded_file, st.baseline_file),
          'individual': ratio(st.expanded_payload_individual,
                              st.baseline_payload_individual),
          'compact': ratio(st.expanded_payload_compact,
                           st.baseline_payload_compact),
      },
      'byte_share': {
          'individual': round(
              100.0 * st.baseline_payload_individual / st.baseline_payload, 1)
          if st.baseline_payload else 0.0,
          'compact': round(
              100.0 * st.baseline_payload_compact / st.baseline_payload, 1)
          if st.baseline_payload else 0.0,
      },
      'mean_bytes_per_event_expanded': round(
          st.expanded_payload / st.total_events, 1) if st.total_events else 0.0,
      'scenarios': scenarios,
      'histogram_top': hist[:top_n],
  }


def mb(n):
  return n / (1024.0 * 1024.0)


def print_human(r):
  b, a = r['bytes'], r['amplification']
  print('=' * 72)
  print(f"ftrace de-bundling — Task 01   [{r['label']}]")
  print('=' * 72)
  ev = r['events']
  print(f"bundles {r['bundles']:,}   lost_events bundles {r['lost_events_bundles']:,}")
  print(f"events  total {ev['total']:,}  (individual {ev['individual']:,}, "
        f"sched_switch {ev['compact_switch']:,}, sched_waking {ev['compact_waking']:,})")
  print('-' * 72)
  print(f"                  {'baseline':>11} {'expanded':>11}   amp")
  print(f"  individual    {mb(b['baseline_individual']):9.2f}MB "
        f"{mb(b['expanded_individual']):9.2f}MB  {a['individual']}x")
  print(f"  compactSched  {mb(b['baseline_compact']):9.2f}MB "
        f"{mb(b['expanded_compact']):9.2f}MB  {a['compact']}x")
  print(f"  TOTAL payload {mb(b['baseline_payload']):9.2f}MB "
        f"{mb(b['expanded_payload']):9.2f}MB  {a['payload']}x")
  print(f"  TOTAL file    {mb(b['baseline_file']):9.2f}MB "
        f"{mb(b['expanded_file']):9.2f}MB  {a['file']}x")
  print(f"  byte share: individual {r['byte_share']['individual']}%  "
        f"compact {r['byte_share']['compact']}%   "
        f"mean {r['mean_bytes_per_event_expanded']} B/event")
  print('-' * 72)
  s = r['scenarios']
  print("  growth of 'what stays in ftrace'  (× data-only / × incl-pkt-header):")
  for key, name in (('all_ftrace', 'all ftrace'),
                    ('excl_print', 'excl print (atrace->SDK)'),
                    ('excl_print_and_sched', 'excl print + sched')):
    sc = s[key]
    print(f"    {name:<26}{sc['today_MB']:7.2f} MB -> "
          f"{sc['growth_data_only']}x data / {sc['growth_with_pkt_framing']}x +pkt"
          f"   [{sc['pct_of_all_bytes']}% of ftrace bytes]")
  print('-' * 72)
  print(f"  top event types by expanded bytes:")
  for h in r['histogram_top']:
    print(f"    {h['name']:<34} {h['count']:>10,}  "
          f"{mb(h['expanded_bytes']):7.2f}MB  {h['pct_expanded']:5.1f}%")
  print('=' * 72)


# field maps for the human-readable --text-sample dump
_SS_FIELDS = {1: 'prev_comm', 2: 'prev_pid', 3: 'prev_prio', 4: 'prev_state',
              5: 'next_comm', 6: 'next_pid', 7: 'next_prio'}
_SW_FIELDS = {1: 'comm', 2: 'pid', 3: 'prio', 4: 'success', 5: 'target_cpu'}
_PRINT_FIELDS = {1: 'ip', 2: 'buf'}  # PrintFtraceEvent (FtraceEvent field 3)
_FE_PRINT = 3


def _esc(b):
  """One-line-safe quoted string (escape control chars so output stays tabular)."""
  s = b.decode('utf-8', 'replace')
  s = (s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
       .replace('\r', '\\r').replace('\t', '\\t'))
  return f'"{s}"'


def pretty_fe(fe, names):
  """Render one (de-bundled) FtraceEvent as a readable, single-line pseudo-proto."""
  head, event = [], ''
  for fnum, wt, val in iter_fields(fe):
    if fnum == F_FE_TIMESTAMP:
      head.append(f'ts={val}')
    elif fnum == F_FE_PID:
      head.append(f'pid={val}')
    elif fnum == F_FE_CPU:
      head.append(f'cpu={val}')
    elif fnum == F_FE_COMMON_FLAGS:
      head.append(f'flags={val}')
    elif wt == 2:  # the event-specific submessage
      ename = names.get(fnum, f'field_{fnum}')
      fmap = (_SS_FIELDS if fnum == F_FE_SCHED_SWITCH else
              _SW_FIELDS if fnum == F_FE_SCHED_WAKING else
              _PRINT_FIELDS if fnum == _FE_PRINT else {})
      inner = []
      for sf, swt, sval in iter_fields(val):
        nm = fmap.get(sf, f'f{sf}')
        inner.append(f'{nm}={_esc(sval)}' if swt == 2 else
                     f'{nm}={to_signed64(sval)}')
      event = f'{ename}{{ {", ".join(inner)} }}'
  # header fields first (aligned-ish), then the event payload
  return f'{" ".join(head):<34} {event}'


def text_sample(input_path, n, names):
  """Print the first ftrace bundle as it is TODAY (summary) and AFTER (each event
  as its own single-event packet, pretty-printed)."""
  data = open(input_path, 'rb').read()
  prev_chain = {}
  for fnum, wt, packet in iter_fields(data):
    if fnum != F_TRACE_PACKET or wt != 2:
      continue
    bundle = None
    for pf, pwt, pval in iter_fields(packet):
      if pf == F_TP_FTRACE_EVENTS and pwt == 2:
        bundle = pval
    if bundle is None:
      continue
    cpu, indiv, cs = 0, [], None
    for bf, bwt, bval in iter_fields(bundle):
      if bf == F_FEB_CPU and bwt == 0:
        cpu = bval
      elif bf == F_FEB_EVENT and bwt == 2:
        indiv.append(bval)
      elif bf == F_FEB_COMPACT_SCHED and bwt == 2:
        cs = bval
    nsw = nwk = 0
    if cs is not None:
      cols = {}
      for f2, w2, v2 in iter_fields(cs):
        if w2 == 2 and f2 != F_CS_INTERN_TABLE:
          cols[f2] = decode_packed_varints(v2)
      nsw = len(cols.get(F_CS_SWITCH_TIMESTAMP, []))
      nwk = len(cols.get(F_CS_WAKING_TIMESTAMP, []))
    print(f"TODAY  — 1 FtraceEventBundle (cpu={cpu}): {len(indiv)} individual "
          f"event(s) in `repeated FtraceEvent event`, plus compact_sched "
          f"{{{nsw} sched_switch, {nwk} sched_waking}} stored columnar.")
    print(f"AFTER  — each becomes its own single-event TracePacket:")
    half = max(1, n // 2)
    if indiv:
      print(f"  [individual events — re-wrapped, content unchanged]")
      for ev in indiv[:half]:
        print('    ' + pretty_fe(ev + field_varint(F_FE_CPU, cpu), names))
    if cs is not None:
      print(f"  [sched events — REBUILT from compact_sched columns "
            f"(full fields incl. prev_*)]")
      shown = 0
      for fe in reconstruct_compact_sched(cs, cpu, prev_chain, Stats()):
        if shown >= n - half:
          break
        print('    ' + pretty_fe(fe, names))
        shown += 1
    return
  print('(no ftrace bundle found)')


def write_profilable(input_path, out_path):
  """Write a de-bundled trace that `trace_processor` CAN parse, for proto-content
  profiling. Each event goes in its OWN 1-event `FtraceEventBundle` — the event
  content is identical to the v2 form (sched re-materialized, no CompactSched);
  the bundle is just a wrapper trace_processor understands, so
  `--analyze-trace-proto-content` works. `cpu` sits on the bundle (as today), not
  on the event. Caveat: this adds a small per-event bundle wrapper that the real
  v2 (`TracePacket.ftrace_event`) wouldn't — the *event field* distribution is
  faithful, the bundle-overhead line is not."""
  data = open(input_path, 'rb').read()
  out = bytearray()
  prev_chain = {}
  dummy = Stats()
  for fnum, wt, packet in iter_fields(data):
    if fnum != F_TRACE_PACKET or wt != 2:
      continue
    bundle = seq = None
    for pf, pwt, pval in iter_fields(packet):
      if pf == F_TP_FTRACE_EVENTS and pwt == 2:
        bundle = pval
      elif pf == F_TP_SEQ_ID and pwt == 0:
        seq = pval
    if bundle is None:
      continue
    cpu, indiv, cs = 0, [], None
    for bf, bwt, bval in iter_fields(bundle):
      if bf == F_FEB_CPU and bwt == 0:
        cpu = bval
      elif bf == F_FEB_EVENT and bwt == 2:
        indiv.append(bval)
      elif bf == F_FEB_COMPACT_SCHED and bwt == 2:
        cs = bval

    def emit(ev_bytes):
      one_bundle = field_varint(F_FEB_CPU, cpu) + field_bytes(F_FEB_EVENT, ev_bytes)
      body = field_bytes(F_TP_FTRACE_EVENTS, one_bundle)
      if seq is not None:
        body += field_varint(F_TP_SEQ_ID, seq)
      out.extend(field_bytes(F_TRACE_PACKET, body))

    for ev in indiv:
      emit(ev)
    if cs is not None:
      for fe in reconstruct_compact_sched(cs, cpu, prev_chain, dummy, add_cpu=False):
        emit(fe)
  open(out_path, 'wb').write(out)


def main():
  ap = argparse.ArgumentParser(description=__doc__,
                               formatter_class=argparse.RawDescriptionHelpFormatter)
  ap.add_argument('input', help='DECOMPRESSED trace (.pftrace)')
  ap.add_argument('--label', default=None, help='tag for this trace in output')
  ap.add_argument('--top', type=int, default=25, help='histogram size')
  ap.add_argument('--write-profilable', metavar='OUT', default=None,
                  help='write a parseable de-bundled trace (1-event bundles) for '
                       '`trace_processor --analyze-trace-proto-content`, then exit')
  ap.add_argument('--text-sample', type=int, default=0, metavar='N',
                  help='print first N de-bundled events (today vs after) and exit')
  ap.add_argument('--json', action='store_true', help='print only JSON blob')
  ap.add_argument('--write-traces', action='store_true',
                  help='also write ftrace_only/expanded.pftrace (for Task 03)')
  ap.add_argument('--out-dir', default=None)
  ap.add_argument('--proto', default=None, help='path to ftrace_event.proto')
  args = ap.parse_args()

  out_dir = args.out_dir or os.path.dirname(os.path.abspath(args.input))
  if args.write_traces:
    os.makedirs(out_dir, exist_ok=True)
  label = args.label or os.path.basename(args.input)
  names = load_event_names(args.proto)
  if args.write_profilable:
    write_profilable(args.input, args.write_profilable)
    print(f'wrote parseable de-bundled trace -> {args.write_profilable}')
    return
  if args.text_sample:
    text_sample(args.input, args.text_sample, names)
    return
  st = analyze(args.input, args.write_traces, out_dir)
  result = build_result(st, label, names, args.top)
  result['_event_names_resolved'] = bool(names)

  if not args.json:
    print_human(result)
    print('\n----- paste the block below back for cross-trace analysis -----')
  print('===JSON BEGIN===')
  print(json.dumps(result, indent=2))
  print('===JSON END===')


if __name__ == '__main__':
  main()
