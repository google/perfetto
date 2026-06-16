#!/usr/bin/env python3
"""
Build a rich multi-dump heap graph fixture with deep reference chains so
the flamegraph diff exposes interesting structure across many levels.

Two snapshots of the same process (pid 2, "system_server") at ts=100 and
ts=2_000_000_100. Between snapshots:

  * Activity / Fragment counts shrink (user closed UI).
  * Background-service Workers grow (long-running jobs accumulated).
  * DataCache / CacheEntry shrink (eviction).
  * Bitmap / SocketBuffer grow (more in-flight network responses).
  * NewlyAddedClass appears in dump 2.
  * RemovedClass disappears.

The class tree is ~10 levels deep, with branching across UI and Service
sub-trees, so the flamegraph has plenty of stacks at every metric (Object
Size, Object Count, Dominated Object Size, Dominated Object Count).
"""

import os
import textwrap

# Class catalogue. id -> (class_name, object_size). Stable across dumps.
CLASSES = [
    (1, 'java.lang.Object', 16),
    (2, 'android.app.Application', 64),
    (3, 'android.app.ActivityManager', 64),
    (4, 'android.app.Activity', 128),
    (5, 'android.app.FragmentManager', 64),
    (6, 'android.app.Fragment', 96),
    (7, 'android.app.BackStack', 32),
    (8, 'android.app.BackStackEntry', 48),
    (9, 'android.view.ViewHolder', 64),
    (10, 'android.widget.TextView', 96),
    (11, 'android.widget.ImageView', 96),
    (12, 'android.graphics.Bitmap', 256),
    (13, 'java.lang.String', 40),
    (14, 'androidx.lifecycle.ViewModel', 80),
    (15, 'com.example.DataCache', 64),
    (16, 'com.example.CacheEntry', 48),
    (17, 'byte[]', 32),  # array; size is metadata, real bytes per-instance
    (18, 'com.example.ApiClient', 64),
    (19, 'com.example.HttpResponse', 64),
    (20, 'android.app.ServiceManager', 64),
    (21, 'com.example.BackgroundService', 64),
    (22, 'com.example.Worker', 64),
    (23, 'com.example.Task', 48),
    (24, 'com.example.TaskData', 48),
    (25, 'com.example.Scheduler', 32),
    (26, 'com.example.ScheduledTask', 48),
    (27, 'com.example.EventBus', 32),
    (28, 'com.example.EventListener', 48),
    (29, 'android.net.ConnectivityService', 64),
    (30, 'android.net.ConnectionPool', 32),
    (31, 'android.net.Connection', 64),
    (32, 'android.net.SocketBuffer', 192),
    (33, 'com.example.RemovedClass', 64),  # only dump 1
    (34, 'com.example.NewlyAddedClass', 64),  # only dump 2
]

# Class-name → id reverse for ergonomic node creation.
CLS = {name: id_ for id_, name, _ in CLASSES}
# Self-size lookup.
SIZE = {id_: sz for id_, _, sz in CLASSES}


class Graph:
  """Builds an object graph for one heap dump.

    Each node has a unique numeric id. Edges are labeled with field name
    "ref" (field_name_id=1). The roots list holds the ROOT_JAVA_FRAME
    object ids."""

  def __init__(self, base):
    self.base = base
    self.next_id = base
    self.objects = []  # list of (id, type_id, self_size, [child_ids])
    self.roots = []

  def new(self, type_id, self_size=None, root=False):
    oid = self.next_id
    self.next_id += 1
    self.objects.append([oid, type_id, self_size or SIZE[type_id], []])
    if root:
      self.roots.append(oid)
    return oid

  def link(self, parent_id, child_id):
    for o in self.objects:
      if o[0] == parent_id:
        o[3].append(child_id)
        return
    raise KeyError(parent_id)


def build_app_graph(g, scale):
  """Build a layered Android-app-style object graph rooted at Application.

    `scale` is a dict that lets each named branch grow/shrink between
    dumps without touching this code. Higher numbers → more instances
    (and therefore wider flamegraph for that branch).
    """
  app = g.new(CLS['android.app.Application'], root=True)

  # ---- UI subtree -------------------------------------------------------
  am = g.new(CLS['android.app.ActivityManager'])
  g.link(app, am)
  for _ in range(scale['activities']):
    act = g.new(CLS['android.app.Activity'])
    g.link(am, act)
    fm = g.new(CLS['android.app.FragmentManager'])
    g.link(act, fm)
    for _ in range(scale['fragments_per_activity']):
      frag = g.new(CLS['android.app.Fragment'])
      g.link(fm, frag)
      for _ in range(scale['holders_per_fragment']):
        vh = g.new(CLS['android.view.ViewHolder'])
        g.link(frag, vh)
        for _ in range(scale['textviews_per_holder']):
          tv = g.new(CLS['android.widget.TextView'])
          g.link(vh, tv)
          for _ in range(scale['strings_per_textview']):
            s = g.new(CLS['java.lang.String'])
            g.link(tv, s)
        for _ in range(scale['imageviews_per_holder']):
          iv = g.new(CLS['android.widget.ImageView'])
          g.link(vh, iv)
          for _ in range(scale['bitmaps_per_imageview']):
            b = g.new(CLS['android.graphics.Bitmap'])
            g.link(iv, b)
      vm = g.new(CLS['androidx.lifecycle.ViewModel'])
      g.link(frag, vm)
      for _ in range(scale['caches_per_viewmodel']):
        dc = g.new(CLS['com.example.DataCache'])
        g.link(vm, dc)
        for _ in range(scale['entries_per_cache']):
          ce = g.new(CLS['com.example.CacheEntry'])
          g.link(dc, ce)
          ba = g.new(CLS['byte[]'], self_size=128)
          g.link(ce, ba)
      for _ in range(scale['apis_per_viewmodel']):
        api = g.new(CLS['com.example.ApiClient'])
        g.link(vm, api)
        for _ in range(scale['resps_per_api']):
          r = g.new(CLS['com.example.HttpResponse'])
          g.link(api, r)
          ba = g.new(CLS['byte[]'], self_size=512)
          g.link(r, ba)
    bs = g.new(CLS['android.app.BackStack'])
    g.link(fm, bs)
    for _ in range(scale['backstack_entries']):
      bse = g.new(CLS['android.app.BackStackEntry'])
      g.link(bs, bse)

  # ---- Service subtree -------------------------------------------------
  sm = g.new(CLS['android.app.ServiceManager'])
  g.link(app, sm)
  for _ in range(scale['bgservices']):
    svc = g.new(CLS['com.example.BackgroundService'])
    g.link(sm, svc)
    for _ in range(scale['workers_per_service']):
      w = g.new(CLS['com.example.Worker'])
      g.link(svc, w)
      for _ in range(scale['tasks_per_worker']):
        t = g.new(CLS['com.example.Task'])
        g.link(w, t)
        td = g.new(CLS['com.example.TaskData'])
        g.link(t, td)
        ba = g.new(CLS['byte[]'], self_size=64)
        g.link(td, ba)
      sched = g.new(CLS['com.example.Scheduler'])
      g.link(w, sched)
      for _ in range(scale['scheduled_per_scheduler']):
        st = g.new(CLS['com.example.ScheduledTask'])
        g.link(sched, st)
    eb = g.new(CLS['com.example.EventBus'])
    g.link(svc, eb)
    for _ in range(scale['listeners_per_eventbus']):
      el = g.new(CLS['com.example.EventListener'])
      g.link(eb, el)
  cs = g.new(CLS['android.net.ConnectivityService'])
  g.link(sm, cs)
  cp = g.new(CLS['android.net.ConnectionPool'])
  g.link(cs, cp)
  for _ in range(scale['connections']):
    c = g.new(CLS['android.net.Connection'])
    g.link(cp, c)
    for _ in range(scale['buffers_per_connection']):
      sb = g.new(CLS['android.net.SocketBuffer'])
      g.link(c, sb)


# Profile for dump 1 (busy UI, lighter network).
SCALE_1 = dict(
    activities=4,
    fragments_per_activity=3,
    holders_per_fragment=4,
    textviews_per_holder=2,
    strings_per_textview=2,
    imageviews_per_holder=1,
    bitmaps_per_imageview=2,
    caches_per_viewmodel=2,
    entries_per_cache=4,
    apis_per_viewmodel=1,
    resps_per_api=2,
    backstack_entries=3,
    bgservices=2,
    workers_per_service=2,
    tasks_per_worker=2,
    scheduled_per_scheduler=2,
    listeners_per_eventbus=3,
    connections=2,
    buffers_per_connection=2,
)
# Profile for dump 2 — UI mostly closed, services accumulated, network heavy.
SCALE_2 = dict(
    activities=2,  # ↓ user closed two activities
    fragments_per_activity=2,  # ↓ fewer fragments
    holders_per_fragment=4,
    textviews_per_holder=2,
    strings_per_textview=2,
    imageviews_per_holder=1,
    bitmaps_per_imageview=4,  # ↑ bigger image cache held longer
    caches_per_viewmodel=1,  # ↓ caches evicted
    entries_per_cache=2,  # ↓ evicted further
    apis_per_viewmodel=2,  # ↑ more api clients
    resps_per_api=4,  # ↑ many in-flight responses
    backstack_entries=2,
    bgservices=3,  # ↑ extra service started
    workers_per_service=4,  # ↑ workers piled up
    tasks_per_worker=3,  # ↑ tasks queued
    scheduled_per_scheduler=4,  # ↑ schedule grew
    listeners_per_eventbus=5,  # ↑ more subscribers
    connections=4,  # ↑ extra outgoing connections
    buffers_per_connection=4,  # ↑ traffic up
)


def emit_classes(out):
  out.append(
      '    location_names {\n      iid: 1\n      str: "/system/framework/test.apk"\n    }'
  )
  out.append('    field_names {\n      iid: 1\n      str: "ref"\n    }')
  for id_, name, size in CLASSES:
    # Make every concrete class extend Object for a one-deep type
    # hierarchy — keeps trace_processor happy.
    sup = '\n      superclass_id: 1' if id_ != 1 else ''
    out.append(
        textwrap.dedent(f'''
            types {{
              id: {id_}
              class_name: "{name}"
              location_id: 1
              object_size: {size}{sup}
            }}''').strip())


def emit_dump(g, ts, dump_label):
  """Emit one heap_graph packet."""
  out = []
  out.append('# === ' + dump_label + ' ===')
  out.append('packet {')
  out.append(f'  trusted_packet_sequence_id: 999')
  out.append(f'  timestamp: {ts}')
  out.append('  incremental_state_cleared: true')
  out.append('  heap_graph {')
  out.append('    pid: 2')
  cls_lines = []
  emit_classes(cls_lines)
  out.append(textwrap.indent('\n'.join(cls_lines), '    '))
  # Roots
  out.append('    roots {')
  out.append('      root_type: ROOT_JAVA_FRAME')
  for rid in g.roots:
    out.append(f'      object_ids: {rid}')
  out.append('    }')
  # Objects
  for oid, type_id, self_size, kids in g.objects:
    out.append('    objects {')
    out.append(f'      id: {oid}')
    out.append(f'      type_id: {type_id}')
    out.append(f'      self_size: {self_size}')
    for _ in kids:
      out.append(f'      reference_field_id: 1')
    for kid in kids:
      out.append(f'      reference_object_id: {kid}')
    out.append('    }')
  out.append('    continued: false')
  out.append('    index: 0')
  out.append('  }')
  out.append('}')
  return '\n'.join(out)


def main():
  out = []
  out.append('packet {')
  out.append('  process_tree {')
  out.append('    processes {')
  out.append('      pid: 2')
  out.append('      ppid: 1')
  out.append('      cmdline: "system_server"')
  out.append('      uid: 1000')
  out.append('    }')
  out.append('  }')
  out.append('}')

  # --- Dump 1 ---
  g1 = Graph(base=0x1000)
  build_app_graph(g1, SCALE_1)
  # Add RemovedClass instances dangling off the app for "GONE in dump 2" demo.
  app1 = g1.roots[0]
  for _ in range(8):
    rc = g1.new(CLS['com.example.RemovedClass'])
    g1.link(app1, rc)
  out.append(emit_dump(g1, ts=100, dump_label='DUMP 1 (initial)'))

  # --- Dump 2 ---
  g2 = Graph(base=0x10000)
  build_app_graph(g2, SCALE_2)
  # Add NewlyAddedClass instances for "NEW in dump 2".
  app2 = g2.roots[0]
  for _ in range(6):
    nc = g2.new(CLS['com.example.NewlyAddedClass'])
    g2.link(app2, nc)
  out.append(emit_dump(g2, ts=2_000_000_100, dump_label='DUMP 2 (later)'))

  txt = '\n'.join(out) + '\n'

  out_path = '/tmp/hprof_test/multi_dump_rich.textproto'
  with open(out_path, 'w') as f:
    f.write(txt)
  print(
      f'wrote {out_path} ({len(txt)} bytes, {len(g1.objects)+len(g2.objects)} objects)'
  )


if __name__ == '__main__':
  main()
