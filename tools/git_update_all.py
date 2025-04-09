#!/usr/bin/env python3
import argparse
import sys
from typing import Dict, List, Optional, Set
import os

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.tools.git_utils import (
    run_git_command,
    topological_sort_branches,
    get_current_branch,
)
#pylint: enable=wrong-import-position


def main():
  parser = argparse.ArgumentParser(
      description='Updates ALL stacked branches via merges based on parent config.'
  )
  args = parser.parse_args()

  print("--- Starting update for ALL stacks ---")
  sorted_branches: List[str] = []
  graph: Dict[str, Optional[str]] = {}
  try:
    sorted_branches, graph = topological_sort_branches()
  except ValueError as e:  # Cycle detected
    print(f"Error: {e}", file=sys.stderr)
    print("Cannot update due to cycles.", file=sys.stderr)
    sys.exit(1)
  except Exception as e:
    print(f"Dependency analysis error: {e}", file=sys.stderr)
    sys.exit(1)

  if not sorted_branches:
    print("No branches with parent configurations found.")
    sys.exit(0)

  print(f"Branches to update (topological order): {', '.join(sorted_branches)}")
  original_branch_to_restore = get_current_branch()

  for branch in sorted_branches:
    parent = graph.get(branch)
    if parent:
      print(f"\nUpdating '{branch}' by merging '{parent}'...")
      try:
        run_git_command(['checkout', branch])
        run_git_command(['merge', parent])
        print(f"Merge successful.")
      except SystemExit as e:  # Merge conflict
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
  print("\n--- Update process finished for ALL stacks ---")


if __name__ == "__main__":
  main()
