#!/usr/bin/env python3
"""Maximize context-switch rate: per-CPU-pinned pipe ping-pong pairs.

Each pair = two processes pinned to the same CPU, ping-ponging a byte. Every
round forces a wakeup + context switch on that CPU -> very high sched_switch /
sched_waking rate. Runs for <dur> seconds then exits.

Usage: python3 sched_load.py <dur_seconds> [num_pairs]
"""
import os
import sys
import time


def pair(cpu, deadline):
  r1, w1 = os.pipe()  # parent -> child
  r2, w2 = os.pipe()  # child  -> parent
  pid = os.fork()
  if pid == 0:  # child = pong
    try:
      os.sched_setaffinity(0, {cpu})
    except OSError:
      pass
    os.close(w1)
    os.close(r2)
    while time.time() < deadline:
      if not os.read(r1, 1):
        break
      try:
        os.write(w2, b'1')
      except BrokenPipeError:
        break
    os._exit(0)
  else:  # parent = ping
    try:
      os.sched_setaffinity(0, {cpu})
    except OSError:
      pass
    os.close(r1)
    os.close(w2)
    try:
      os.write(w1, b'1')  # kick off
      while time.time() < deadline:
        if not os.read(r2, 1):
          break
        os.write(w1, b'1')
    except (BrokenPipeError, OSError):
      pass
    try:
      os.close(w1)
    except OSError:
      pass
    try:
      os.waitpid(pid, 0)
    except ChildProcessError:
      pass


def main():
  dur = float(sys.argv[1])
  ncpu = os.cpu_count()
  npairs = int(sys.argv[2]) if len(sys.argv) > 2 else ncpu * 4
  deadline = time.time() + dur
  kids = []
  for i in range(npairs):
    pid = os.fork()
    if pid == 0:
      pair(i % ncpu, deadline)
      os._exit(0)
    kids.append(pid)
  for pid in kids:
    try:
      os.waitpid(pid, 0)
    except ChildProcessError:
      pass


if __name__ == '__main__':
  main()
