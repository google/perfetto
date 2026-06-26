#!/usr/bin/env python3
"""Self-test ftrace_expand.py on a hand-built synthetic trace."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ftrace_expand as fx


def build_compact_sched():
  # 2 switch events, 1 waking event. intern_table=[swapper, foo, bar]
  intern = [b'swapper', b'foo', b'bar']
  cs = b''
  for s in intern:
    cs += fx.field_string(fx.F_CS_INTERN_TABLE, s)
  # packed columns
  def packed(field, vals):
    blob = b''.join(fx.encode_varint(v) for v in vals)
    return fx.field_bytes(field, blob)
  cs += packed(fx.F_CS_SWITCH_TIMESTAMP, [1000, 50])      # abs 1000, then +50 -> 1050
  cs += packed(fx.F_CS_SWITCH_PREV_STATE, [0, 1])
  cs += packed(fx.F_CS_SWITCH_NEXT_PID, [101, 102])
  cs += packed(fx.F_CS_SWITCH_NEXT_PRIO, [120, 120])
  cs += packed(fx.F_CS_SWITCH_NEXT_COMM_INDEX, [1, 2])    # foo, then bar
  cs += packed(fx.F_CS_WAKING_TIMESTAMP, [2000])
  cs += packed(fx.F_CS_WAKING_PID, [202])
  cs += packed(fx.F_CS_WAKING_TARGET_CPU, [3])
  cs += packed(fx.F_CS_WAKING_PRIO, [120])
  cs += packed(fx.F_CS_WAKING_COMM_INDEX, [2])            # bar
  cs += packed(fx.F_CS_WAKING_COMMON_FLAGS, [1])
  return cs


def build_individual_event():
  # FtraceEvent{timestamp=900, pid=7, print=<bytes>} (use field 3 = print)
  return (fx.field_varint(fx.F_FE_TIMESTAMP, 900) +
          fx.field_varint(fx.F_FE_PID, 7) +
          fx.field_bytes(3, b'\x0a\x03abc'))  # print FtraceEvent, opaque-ish


def build_trace():
  bundle = (fx.field_varint(fx.F_FEB_CPU, 3) +
            fx.field_bytes(fx.F_FEB_EVENT, build_individual_event()) +
            fx.field_bytes(fx.F_FEB_COMPACT_SCHED, build_compact_sched()))
  packet = (fx.field_bytes(fx.F_TP_FTRACE_EVENTS, bundle) +
            fx.field_varint(fx.F_TP_SEQ_ID, 5))
  trace = fx.field_bytes(fx.F_TRACE_PACKET, packet)
  return trace


def parse_expanded(path):
  """Return list of (timestamp, cpu, kind, sched_fields) for each packet."""
  data = open(path, 'rb').read()
  events = []
  for fnum, wt, packet in fx.iter_fields(data):
    assert fnum == fx.F_TRACE_PACKET
    fe = None
    for pf, pwt, pval in fx.iter_fields(packet):
      if pf == fx.F_TP_FTRACE_EVENT and pwt == 2:
        fe = pval
    ts = cpu = None
    kind = 'other'
    sched = {}
    for ff, fwt, fval in fx.iter_fields(fe):
      if ff == fx.F_FE_TIMESTAMP:
        ts = fval
      elif ff == fx.F_FE_CPU:
        cpu = fval
      elif ff == fx.F_FE_SCHED_SWITCH:
        kind = 'switch'
        for sf, swt, sval in fx.iter_fields(fval):
          sched[sf] = sval
      elif ff == fx.F_FE_SCHED_WAKING:
        kind = 'waking'
        for sf, swt, sval in fx.iter_fields(fval):
          sched[sf] = sval
    events.append((ts, cpu, kind, sched))
  return events


def main():
  trace = build_trace()
  inp = '/tmp/synthetic.pftrace'
  open(inp, 'wb').write(trace)
  stats = fx.analyze(inp, True, '/tmp')
  exp = '/tmp/ftrace_expanded.pftrace'

  # --- assertions on counts ---
  assert stats.bundles == 1, stats.bundles
  assert stats.individual_events == 1, stats.individual_events
  assert stats.compact_switch == 2, stats.compact_switch
  assert stats.compact_waking == 1, stats.compact_waking
  assert stats.total_events == 4, stats.total_events

  events = parse_expanded(exp)
  assert len(events) == 4, len(events)

  # individual event: ts=900, cpu appended=3
  indiv = [e for e in events if e[2] == 'other']
  assert len(indiv) == 1
  assert indiv[0][0] == 900 and indiv[0][1] == 3, indiv[0]

  # sched_switch events
  switches = [e for e in events if e[2] == 'switch']
  assert len(switches) == 2
  # timestamps absolute: 1000, 1050
  assert switches[0][0] == 1000, switches[0][0]
  assert switches[1][0] == 1050, switches[1][0]
  # cpu = 3 on both
  assert all(e[1] == 3 for e in switches)
  # switch[0]: next_comm='foo'(field5), next_pid=101(field6); prev_* empty (first on cpu)
  s0 = switches[0][3]
  assert s0[fx.F_SS_NEXT_COMM] == b'foo', s0
  assert s0[fx.F_SS_NEXT_PID] == 101, s0
  assert s0[fx.F_SS_PREV_PID] == 0, s0  # first on cpu -> 0
  # switch[1]: prev_* CHAINED from switch[0].next (foo/101/120)
  s1 = switches[1][3]
  assert s1[fx.F_SS_PREV_COMM] == b'foo', s1
  assert s1[fx.F_SS_PREV_PID] == 101, s1
  assert s1[fx.F_SS_PREV_PRIO] == 120, s1
  assert s1[fx.F_SS_NEXT_COMM] == b'bar', s1
  assert s1[fx.F_SS_NEXT_PID] == 102, s1

  # sched_waking
  wakings = [e for e in events if e[2] == 'waking']
  assert len(wakings) == 1
  w = wakings[0]
  assert w[0] == 2000 and w[1] == 3, w
  assert w[3][fx.F_SW_COMM] == b'bar', w[3]
  assert w[3][fx.F_SW_PID] == 202, w[3]
  assert w[3][fx.F_SW_TARGET_CPU] == 3, w[3]

  # payload accounting sanity
  assert stats.expanded_payload == (stats.expanded_payload_individual +
                                    stats.expanded_payload_compact)
  assert stats.baseline_payload > 0 and stats.expanded_payload > 0
  # histogram: sched_switch x2, sched_waking x1, and the individual print event
  bt = stats.by_type
  assert bt['sched_switch'][0] == 2 and bt['sched_waking'][0] == 1, bt
  print('ALL ASSERTIONS PASSED')


if __name__ == '__main__':
  main()
