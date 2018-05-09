#!/usr/bin/python

# Copyright (C) 2018 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import sys

import numpy as np
from matplotlib import pyplot as plt

from absl import app
from absl import flags

FLAGS = flags.FLAGS

flags.DEFINE_integer('window', 100, 'Size of rolling average window')

COLORS = ['b', 'g', 'r', 'c', 'm', 'y', 'k', 'indigo']


def max_default(seq, default):
  try:
    return np.max(seq)
  except ValueError:
    return default


def main(argv):
  max_val = 0
  max_key = ""

  n = 0
  for fn in argv[1:]:
    name = os.path.basename(fn)
    xs = np.loadtxt(fn, dtype=np.int)
    ys = np.arange(len(xs))

    delta = ys - np.array([max_default(ys[xs < x - FLAGS.window], np.NaN)
                           for x in xs])

    max_delta = np.nanmax(delta)
    if max_delta > max_val:
      max_val = max_delta
      max_key = name

    plt.plot(xs, delta, color=COLORS[n % len(COLORS)], label=name)
    print xs, delta
    n += 1
  print "Max delta %d in %s" % (max_val, max_key)
  print "Buffer size: %d KB" % (max_val * 4)
  plt.legend()
  plt.show()


if __name__ == '__main__':
  app.run(main)
