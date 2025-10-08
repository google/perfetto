#!/usr/bin/env python3
# git_utils.py
import subprocess
import sys
import os
from typing import List, Optional, Set, Dict, Tuple, Deque
from collections import deque

# Mainline branches that are treated as valid parents even though they're remote refs
MAINLINE_BRANCHES = {'origin/main'}


def run_command(
    cmd: List[str],
    check: bool = True,
    **kwargs,
) -> subprocess.CompletedProcess:
  """Runs an external command, handles basic errors, ensures consistent output env."""
  if not cmd:
    raise ValueError("Command list empty.")
  executable = cmd[0]
  try:
    env = kwargs.pop('env', {})
    current_env = {**os.environ, 'LC_ALL': 'C', **env}
    return subprocess.run(
        cmd,
        check=check,
        capture_output=True,
        text=True,
        encoding='utf-8',
        errors='replace',
        env=current_env,
        **kwargs)
  except subprocess.CalledProcessError as e:
    if check:  # Only print/exit if check=True caused the error
      print(f"Error running: {' '.join(cmd)}", file=sys.stderr)
      if e.stderr:
        print(f"Stderr:\n{e.stderr.strip()}", file=sys.stderr)
      if e.stdout:
        print(f"Stdout:\n{e.stdout.strip()}", file=sys.stderr)
      sys.exit(e.returncode)
    else:
      raise  # Re-raise if check=False so caller knows it failed
  except FileNotFoundError:
    print(f"Error: '{executable}' not found.", file=sys.stderr)
    sys.exit(1)
  except Exception as e:
    print(f"Error running {' '.join(cmd)}: {e}", file=sys.stderr)
    sys.exit(1)


def run_git_command(args: List[str],
                    check: bool = True,
                    **kwargs) -> subprocess.CompletedProcess:
  """Wrapper to run Git commands using run_command."""
  return run_command(['git'] + args, check=check, **kwargs)


def get_current_branch() -> Optional[str]:
  """Gets the current Git branch name, returns None if detached HEAD."""
  try:
    result = run_git_command(['symbolic-ref', '--short', 'HEAD'], check=False)
    if result.returncode == 0:
      return result.stdout.strip()
    result = run_git_command(['branch', '--show-current'], check=False)
    if result.returncode == 0:
      branch = result.stdout.strip()
      return branch if branch else None
    return None
  except Exception:
    return None


def get_branch_parent(branch_name: str) -> Optional[str]:
  """Gets the configured parent of a branch from git config."""
  if not branch_name:
    return None
  result = run_git_command(['config', f'branch.{branch_name}.parent'],
                           check=False)
  if result.returncode == 0 and result.stdout.strip():
    return result.stdout.strip()
  return None


def get_all_branches() -> List[str]:
  """Gets a list of all local branch names."""
  result = run_git_command(
      ['for-each-ref', '--format=%(refname:short)', 'refs/heads/'])
  return result.stdout.strip().split('\n') if result.stdout.strip() else []


def get_branch_children(parent_name: str,
                        all_local_branches: List[str]) -> List[str]:
  """Finds all branches listing parent_name as their configured parent."""
  children = []
  for branch in all_local_branches:
    if get_branch_parent(branch) == parent_name:
      children.append(branch)
  return children


def get_upstream_branch_name(branch_name: str) -> Optional[str]:
  """
    Gets the simple name (e.g., 'feature-x') of the upstream branch for the given local branch
    using 'git rev-parse --abbrev-ref branch@{u}'.
    Returns None if no upstream is configured or output is unexpected.
    """
  # Use rev-parse with @{u} (short for @{upstream})
  # check=False needed because it fails if no upstream is set
  result = run_git_command(
      ['rev-parse', '--abbrev-ref', f'{branch_name}@{{u}}'], check=False)

  if result.returncode == 0 and result.stdout.strip():
    upstream_full_name = result.stdout.strip(
    )  # e.g., origin/main or origin/feature-x
    parts = upstream_full_name.split('/', 1)
    if len(parts) == 2 and parts[1]:
      remote_name, base_name = parts
      return base_name  # Return just the branch name part
    else:
      print(
          f"Warning: Unexpected upstream format '{upstream_full_name}' for branch '{branch_name}'. Cannot determine base name.",
          file=sys.stderr)
  return None


def get_ancestors(
    start_branch: str,
    mainline_branches: Set[str],
    all_local_branches: List[str],
) -> List[str]:
  """
    Traces direct lineage upwards via 'parent' config. Returns [parent, ..., base].
    Raises ValueError if a cycle is detected in the path.
    """
  ancestors: List[str] = []
  current = start_branch
  visited_trace = {start_branch}

  while True:
    parent = get_branch_parent(current)
    if not parent:
      break
    if parent in mainline_branches:
      break
    if parent not in all_local_branches:
      break
    if parent in visited_trace:
      path = [start_branch] + ancestors + [parent]
      raise ValueError(
          f"Cycle detected tracing ancestors: ... -> {parent} -> {current}")

    ancestors.append(parent)
    visited_trace.add(parent)
    current = parent

  return ancestors


def get_descendants(
    start_branch: str,
    all_local_branches: List[str],
) -> List[str]:
  """
    Finds all descendants using BFS. Returns list (BFS level order).
    Raises ValueError if a cycle is detected involving descendants.
    """
  descendants: List[str] = []
  direct_children = [
      ch for ch in get_branch_children(start_branch, all_local_branches)
      if ch in all_local_branches
  ]
  queue: Deque[Tuple[str, List[str]]] = deque([
      (child, [start_branch, child]) for child in direct_children
  ])
  visited_bfs: Set[str] = set(direct_children)
  processed_bfs: Set[str] = set()

  while queue:
    current_branch, path = queue.popleft()
    if current_branch in processed_bfs:
      continue
    descendants.append(current_branch)
    processed_bfs.add(current_branch)

    grandchildren = get_branch_children(current_branch, all_local_branches)
    for child in grandchildren:
      if child in path:
        raise ValueError(
            f"Cycle detected tracing descendants: {' -> '.join(path)} -> {child}"
        )
      if child not in visited_bfs and child in all_local_branches:
        visited_bfs.add(child)
        new_path = path + [child]
        queue.append((child, new_path))
  return descendants


def get_connected_branches(
    start_branch: str,
    mainline_branches: Set[str],
    all_local_branches: List[str],
) -> Set[str]:
  """
    Finds the set of branches connected to start_branch (ancestors + start + descendants).
    Raises ValueError if cycles are detected during traversal.
    """
  if start_branch not in all_local_branches:
    return set()
  ancestors = get_ancestors(start_branch, mainline_branches, all_local_branches)
  descendants = get_descendants(start_branch, all_local_branches)
  connected = {start_branch}
  connected.update(ancestors)
  connected.update(descendants)
  return connected


def get_stack_base(
    start_branch: str,
    mainline_branches: Set[str],
    all_local_branches: List[str],
) -> str:
  """Finds the highest ancestor before a mainline branch or root."""
  ancestors = get_ancestors(start_branch, mainline_branches,
                            all_local_branches)  # Raises ValueError on cycle
  base = ancestors[-1] if ancestors else start_branch
  return base


def get_stack_branches_ordered(
    start_branch: str,
    mainline_branches: Set[str],
    all_local_branches: List[str],
) -> List[str]:
  """Finds all branches in stack segment, ordered parent-first via BFS from base."""
  stack_base = get_stack_base(start_branch, mainline_branches,
                              all_local_branches)  # Raises ValueError on cycle

  ordered_stack: List[str] = []
  queue: Deque[str] = deque()
  visited_bfs: Set[str] = set()

  if stack_base in all_local_branches:
    queue.append(stack_base)
    visited_bfs.add(stack_base)
  else:
    return []

  processed_bfs: Set[str] = set()

  while queue:
    current_branch = queue.popleft()
    if current_branch in processed_bfs:
      continue
    ordered_stack.append(current_branch)
    processed_bfs.add(current_branch)

    children = get_branch_children(current_branch, all_local_branches)
    for child in children:
      if child not in visited_bfs and child in all_local_branches:
        visited_bfs.add(child)
        queue.append(child)

  if start_branch not in visited_bfs and start_branch in all_local_branches:
    print(
        f"Warning: Target '{start_branch}' not reached from base '{stack_base}'. Config inconsistent?",
        file=sys.stderr)

  return ordered_stack


def topological_sort_branches() -> Tuple[List[str], Dict[str, Optional[str]]]:
  """
    Performs a topological sort of branches based on parent config.
    Returns the sorted list (parents before children) and the graph used.
    Raises ValueError if cycles detected.
    """
  all_branches = get_all_branches()
  graph: Dict[str, Optional[str]] = {}
  nodes = set()
  for branch in all_branches:
    parent = get_branch_parent(branch)
    if parent:
      graph[branch] = parent
      nodes.add(branch)
      nodes.add(parent)
    else:
      if any(get_branch_parent(b) == branch for b in all_branches):
        graph[branch] = None
        nodes.add(branch)

  in_degree: Dict[str, int] = {node: 0 for node in nodes}
  adj: Dict[str, List[str]] = {node: [] for node in nodes}
  for child, parent in graph.items():
    if parent in adj:
      adj[parent].append(child)
      in_degree[child] += 1

  queue: Deque[str] = deque([node for node in nodes if in_degree[node] == 0])
  sorted_order: List[str] = []

  while queue:
    parent = queue.popleft()
    sorted_order.append(parent)
    if parent in adj:
      for child in sorted(adj[parent]):
        in_degree[child] -= 1
        if in_degree[child] == 0:
          queue.append(child)

  if len(sorted_order) != len(nodes):
    remaining_nodes = {node for node, degree in in_degree.items() if degree > 0}
    raise ValueError(
        f"Cycle detected during topological sort involving: {remaining_nodes}")

  return sorted_order, graph
