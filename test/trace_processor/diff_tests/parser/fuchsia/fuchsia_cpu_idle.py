#!/usr/bin/env python3
# Copyright 2026 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Emits a Fuchsia trace holding kernel:sched context switches to and from the
# idle thread, in both the weighted and the legacy encoding.

import struct
import sys
from typing import NamedTuple

# String indices interned below.
INCOMING_WEIGHT = 1
OUTGOING_WEIGHT = 2

# Weight identifying the idle thread in weighted context switches.
IDLE_WEIGHT = -2**31

WEIGHTED_CPU = 2
LEGACY_CPU = 5


class WeightedThread(NamedTuple):
  tid: int
  weight: int


class LegacyThread(NamedTuple):
  pid: int
  tid: int
  priority: int


IDLE_WEIGHTED_THREAD = WeightedThread(tid=0, weight=IDLE_WEIGHT)
IDLE_LEGACY_THREAD = LegacyThread(pid=0, tid=0, priority=0)


def push_word(w):
  sys.stdout.buffer.write(struct.pack('<Q', w))


def push_string(index, s):
  s_bytes = s.encode('utf-8')
  s_len = len(s_bytes)
  num_words = 1 + (s_len + 7) // 8
  header = (s_len << 32) | (index << 16) | (num_words << 4) | 2
  push_word(header)
  padded = s_bytes.ljust((num_words - 1) * 8, b'\x00')
  for i in range(0, len(padded), 8):
    push_word(struct.unpack('<Q', padded[i:i + 8])[0])


def int32_arg(name_ref, value):
  return ((value & 0xffffffff) << 32) | (name_ref << 16) | (1 << 4) | 1


def push_context_switch(ts, cpu, outgoing: WeightedThread,
                        incoming: WeightedThread):
  # record_type = 8 (scheduler), event_type = 1 (context switch),
  # outgoing_state = 2, argument_count = 2, 6 words.
  push_word((1 << 60) | (2 << 36) | (cpu << 20) | (2 << 16) | (6 << 4) | 8)
  push_word(ts)
  push_word(outgoing.tid)
  push_word(incoming.tid)
  push_word(int32_arg(INCOMING_WEIGHT, incoming.weight))
  push_word(int32_arg(OUTGOING_WEIGHT, outgoing.weight))


def push_legacy_context_switch(ts, cpu, outgoing: LegacyThread,
                               incoming: LegacyThread):
  # record_type = 8 (scheduler), event_type = 0 (legacy context switch),
  # outgoing_state = 2, inline thread refs, 6 words.
  push_word((0 << 60) | (incoming.priority << 52) | (outgoing.priority << 44)
            | (2 << 24) | (cpu << 16) | (6 << 4) | 8)
  push_word(ts)
  push_word(outgoing.pid)
  push_word(outgoing.tid)
  push_word(incoming.pid)
  push_word(incoming.tid)


def main():
  # Fuchsia trace magic word: 0x0016547846040010
  sys.stdout.buffer.write(b'\x10\x00\x04\x46\x78\x54\x16\x00')

  push_string(INCOMING_WEIGHT, 'incoming_weight')
  push_string(OUTGOING_WEIGHT, 'outgoing_weight')

  # Weighted context switches: the idle thread carries the idle weight.
  # Leaving idle marks the core active.
  push_context_switch(
      1000,
      WEIGHTED_CPU,
      outgoing=IDLE_WEIGHTED_THREAD,
      incoming=WeightedThread(tid=101, weight=0))
  # Switching between two running threads leaves the idle state untouched.
  push_context_switch(
      2000,
      WEIGHTED_CPU,
      outgoing=WeightedThread(tid=101, weight=0),
      incoming=WeightedThread(tid=102, weight=10))
  # Entering idle marks the core idle.
  push_context_switch(
      3000,
      WEIGHTED_CPU,
      outgoing=WeightedThread(tid=102, weight=10),
      incoming=IDLE_WEIGHTED_THREAD)
  # Switching from idle to idle emits nothing.
  push_context_switch(
      3500,
      WEIGHTED_CPU,
      outgoing=IDLE_WEIGHTED_THREAD,
      incoming=IDLE_WEIGHTED_THREAD)

  # Legacy context switches: the idle thread is identified by pid 0 and
  # priority 0.
  push_legacy_context_switch(
      4000,
      LEGACY_CPU,
      outgoing=IDLE_LEGACY_THREAD,
      incoming=LegacyThread(pid=1, tid=501, priority=1))
  # Switching between two running threads leaves the idle state untouched.
  push_legacy_context_switch(
      4500,
      LEGACY_CPU,
      outgoing=LegacyThread(pid=1, tid=501, priority=1),
      incoming=LegacyThread(pid=1, tid=502, priority=1))
  push_legacy_context_switch(
      5000,
      LEGACY_CPU,
      outgoing=LegacyThread(pid=1, tid=502, priority=1),
      incoming=IDLE_LEGACY_THREAD)
  push_legacy_context_switch(
      5500,
      LEGACY_CPU,
      outgoing=IDLE_LEGACY_THREAD,
      incoming=IDLE_LEGACY_THREAD)


if __name__ == '__main__':
  main()
