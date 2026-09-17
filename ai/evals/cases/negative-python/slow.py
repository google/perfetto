#!/usr/bin/env python3
"""Toy script used as a negative control in the Perfetto agent evals."""

import random


def load_records(n):
  rng = random.Random(42)
  return [(rng.randint(0, 5000), rng.random()) for _ in range(n)]


def find_collisions(records):
  # Quadratic on purpose.
  hits = 0
  for i in range(len(records)):
    for j in range(i + 1, len(records)):
      if records[i][0] == records[j][0]:
        hits += 1
  return hits


def summarize(records):
  total = sum(v for _, v in records)
  return total / len(records)


def main():
  records = load_records(6000)
  print('mean', summarize(records))
  print('collisions', find_collisions(records))


if __name__ == '__main__':
  main()
