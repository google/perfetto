#!/usr/bin/env python
# Copyright (C) 2017 The Android Open Source Project
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

""" Mirrors a Gerrit repo into GitHub, turning CLs into individual branches.

This script does a bit of git black magic. It does mainly two things:
1) Mirrors all the branches (refs/heads/foo) from Gerrit to Github as-is, taking
   care of propagating also deletions.
2) Rewrites Gerrit CLs (refs/changes/NN/cl_number/patchset_number) as
   Github branches (refs/heads/cl_number) recreating a linear chain of commits
   for each patchset in any given CL.

2. Is the trickier part. The problem is that Gerrit stores each patchset of
each CL as an independent ref, e.g.:
  $ git ls-remote origin
  94df12f950462b55a2257b89d1fad6fac24353f9	refs/changes/10/496410/1
  4472fadddf8def74fd76a66ff373ca1245c71bcc	refs/changes/10/496410/2
  90b8535da0653d8f072e86cef9891a664f4e9ed7	refs/changes/10/496410/3
  2149c215fa9969bb454f23ce355459f28604c545	refs/changes/10/496410/meta

  53db7261268802648d7f6125ae6242db17e7a60d	refs/changes/20/494620/1
  d25e56930486363e0637b0a9debe3ae3ec805207	refs/changes/20/494620/2

Where each ref is base on top of the master branch (or whatever the dev choose).
On GitHub, instead, we want to recreate something similar to the pull-request
model, ending up with one branch per CL, and one commit per patchset.
Also we want to make them non-hidden branch heads (i.e. in the refs/heads/)
name space, because Travis CI does not hooks hidden branches.
In conclusion we want to transform the above into:

refs/changes/496410
  * commit: [CL 496410, Patchset 3] (parent: [CL 496410, Patchset 2])
  * commit: [CL 496410, Patchset 2] (parent: [CL 496410, Patchset 1])
  * commit: [CL 496410, Patchset 1] (parent: [master])
refs/changes/496420
  * commit: [CL 496420, Patchset 2] (parent: [CL 496420, Patchset 1])
  * commit: [CL 496420, Patchset 1] (parent: [master])

"""

import collections
import os
import re
import shutil
import subprocess
import sys
import time
import traceback


CUR_DIR = os.path.dirname(os.path.abspath(__file__))
GIT_UPSTREAM = 'https://android.googlesource.com/platform/external/perfetto/'
GIT_MIRROR = 'git@github.com:catapult-project/perfetto.git'
WORKDIR = os.path.join(CUR_DIR, 'repo')

# The actual deploy_key is stored into the internal team drive, undef /infra/.
ENV = {'GIT_SSH_COMMAND': 'ssh -i ' + os.path.join(CUR_DIR, 'deploy_key')}


def GitCmd(*args, **kwargs):
  cmd = ['git'] + list(args)
  p = subprocess.Popen(
      cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr,
      cwd=WORKDIR, env=ENV)
  out = p.communicate(kwargs.get('stdin'))[0]
  assert p.returncode == 0, 'FAIL: ' + ' '.join(cmd)
  return out


# Create a git repo that mirrors both the upstream and the mirror repos.
def Setup():
  if os.path.exists(WORKDIR):
    shutil.rmtree(WORKDIR)
  os.makedirs(WORKDIR)
  GitCmd('init', '--bare', '--quiet')
  GitCmd('remote', 'add', 'upstream', GIT_UPSTREAM)
  GitCmd('config', 'remote.upstream.fetch', '+refs/*:refs/remotes/upstream/*')
  GitCmd('remote', 'add', 'mirror', GIT_MIRROR, '--mirror=fetch')


def GetCommit(commit_sha1):
  raw = GitCmd('cat-file', 'commit', commit_sha1)
  return {
    'tree': re.search(r'^tree\s(\w+)$', raw, re.M).group(1),
    'parent': re.search(r'^parent\s(\w+)$', raw, re.M).group(1),
    'author': re.search(r'^author\s(.+)$', raw, re.M).group(1),
    'committer': re.search(r'^committer\s(.+)$', raw, re.M).group(1),
    'message': re.search(r'\n\n(.+)', raw, re.M | re.DOTALL).group(1),
  }


def ForgeCommit(tree, parent, author, committer, message):
  raw = 'tree %s\nparent %s\nauthor %s\ncommitter %s\n\n%s' % (
      tree, parent, author, committer, message)
  out = GitCmd('hash-object', '-w', '-t', 'commit', '--stdin', stdin=raw)
  return out.strip()


def Sync():
  GitCmd('remote', 'update')
  all_refs = GitCmd('show-ref')
  future_heads = {}
  current_heads = {}
  changes = collections.defaultdict(dict)

  # List all refs from both repos and:
  # 1. Keep track of all branch heads refnames and sha1s from the (github)
  #    mirror into |current_heads|.
  # 2. Keep track of all upstream (AOSP) branch heads into |future_heads|. Note:
  #    this includes only pure branches and NOT CLs. CLs and their patchsets are
  #    stored in a hidden ref (refs/changes) which is NOT under refs/heads.
  # 3. Keep track of all upstream (AOSP) CLs from the refs/changes namespace
  #    into changes[cl_number][patchset_number].
  for line in all_refs.splitlines():
    ref_sha1, ref = line.split()

    PREFIX = 'refs/heads/'
    if ref.startswith(PREFIX):
      branch = ref[len(PREFIX):]
      current_heads['refs/heads/' + branch] = ref_sha1
      continue

    PREFIX = 'refs/remotes/upstream/heads/'
    if ref.startswith(PREFIX):
      branch = ref[len(PREFIX):]
      future_heads['refs/heads/' + branch] = ref_sha1
      continue

    PREFIX = 'refs/remotes/upstream/changes/'
    if ref.startswith(PREFIX):
      (_, cl_num, patchset) = ref[len(PREFIX):].split('/')
      if not cl_num.isdigit() or not patchset.isdigit():
        continue
      cl_num, patchset = int(cl_num), int(patchset)
      changes[cl_num][patchset] = ref_sha1

  # Now iterate over the upstream (AOSP) CLS and forge a chain of commits,
  # creating one branch refs/heads/changes/cl_number for each set of patchsets.
  for cl_num, patchsets in changes.iteritems():
    parent_sha1 = None
    for patchset_num, patchset_sha1 in sorted(patchsets.items(), key=lambda x:x[0]):
      patchset_data = GetCommit(patchset_sha1)
      parent_sha1 = parent_sha1 or patchset_data['parent']
      forged_sha1 = ForgeCommit(
          tree=patchset_data['tree'],
          parent=parent_sha1,
          author=patchset_data['author'],
          committer=patchset_data['committer'],
          message='[Patchset %d] %s' % (patchset_num, patchset_data['message']))
      parent_sha1 = forged_sha1
      future_heads['refs/heads/changes/%d' % cl_num] = forged_sha1

  # Now compute:
  # 1. The set of branches in the mirror (github) that have been deleted on the
  #    upstream (AOSP) repo. These will be deleted also from the mirror.
  # 2. The set of rewritten branches to be updated.
  updte_ref_cmd = ''
  for ref_to_delete in set(current_heads) - set(future_heads):
    updte_ref_cmd += 'delete %s\n' % ref_to_delete
  for ref_to_update, ref_sha1 in future_heads.iteritems():
    if current_heads.get(ref_to_update) != ref_sha1:
      updte_ref_cmd += 'update %s %s\n' % (ref_to_update, ref_sha1)
  print updte_ref_cmd

  # Update objects and push.
  GitCmd('update-ref', '--stdin', stdin=updte_ref_cmd)
  GitCmd('push', 'mirror', '--all', '--prune', '--force')
  GitCmd('gc', '--prune=all', '--aggressive', '--quiet')


def Main():
  Setup()
  while True:
    Sync()
    time.sleep(60)


if __name__ == '__main__':
  sys.exit(Main())
