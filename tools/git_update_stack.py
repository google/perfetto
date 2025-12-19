#!/usr/bin/env python3
import argparse
import sys
from typing import Set, List, Deque
from collections import deque
import os

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.tools.git_utils import (
    get_current_branch,
    get_all_branches,
    get_stack_branches_ordered,
    get_branch_parent,
    run_git_command,
    MAINLINE_BRANCHES,
)
#pylint: enable=wrong-import-position


def main():
  parser = argparse.ArgumentParser(
      description=(
          'Updates target stack segment (target + ancestors + descendants) via merges.'
      ),
      formatter_class=argparse.RawTextHelpFormatter)
  parser.add_argument(
      '-t',
      '--target',
      metavar='branch_name',
      default=None,
      help='Target branch stack (default: current)')
  args = parser.parse_args()

  target_branch = args.target or get_current_branch()
  if not target_branch:
    print("Error: Cannot determine target branch.", file=sys.stderr)
    sys.exit(1)

  all_local_branches = get_all_branches()
  if target_branch not in all_local_branches:
    print(
        f"Error: Target branch '{target_branch}' not found locally.",
        file=sys.stderr)
    sys.exit(1)

  ordered_branches: List[str] = []
  try:
    ordered_branches = get_stack_branches_ordered(target_branch,
                                                  MAINLINE_BRANCHES,
                                                  all_local_branches)
  except ValueError as e:
    print(f"Error determining stack order (Cycle?): {e}", file=sys.stderr)
    sys.exit(1)

  if not ordered_branches:
    print("No branches identified for update. Exiting.", file=sys.stderr)
    sys.exit(1)

  print(
      f"Branches to update (order: parent-first): {', '.join(ordered_branches)}"
  )
  original_branch_to_restore = get_current_branch()

  for branch in ordered_branches:
    parent = get_branch_parent(branch)
    if parent:
      print(f"\nUpdating '{branch}' by merging '{parent}'...")
      try:
        run_git_command(['checkout', branch])
        run_git_command(['merge', parent])
        print(f"Merge successful.")
      except SystemExit as e:  # Indicates merge conflict from run_git_command
        print(
            f"MERGE FAILED: Conflicts merging '{parent}' into '{branch}'.",
            file=sys.stderr)
        print("Resolve conflicts manually and commit.", file=sys.stderr)
        if original_branch_to_restore and get_current_branch(
        ) != original_branch_to_restore:
          print(f"Restoring original branch '{original_branch_to_restore}'...")
          run_git_command(['checkout', original_branch_to_restore], check=False)
        sys.exit(e.code if isinstance(e.code, int) else 1)
      except Exception as e:
        print(f"\nUnexpected error updating '{branch}': {e}", file=sys.stderr)
        if original_branch_to_restore and get_current_branch(
        ) != original_branch_to_restore:
          run_git_command(['checkout', original_branch_to_restore], check=False)
        sys.exit(1)

  if original_branch_to_restore and get_current_branch(
  ) != original_branch_to_restore:
    print(f"\nRestoring original branch '{original_branch_to_restore}'...")
    run_git_command(['checkout', original_branch_to_restore], check=False)
  print(
      f"\n--- Update process finished for stack including '{target_branch}' ---"
  )


if __name__ == "__main__":
  main()
