#!/usr/bin/env python3
import argparse
import sys
import os
from typing import Dict, List, Optional, Set

# Allow importing of root-relative modules.
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))

#pylint: disable=wrong-import-position
from python.tools.git_utils import (
    get_all_branches,
    get_branch_children,
    get_current_branch,
    run_git_command,
    topological_sort_branches,
    MAINLINE_BRANCHES,
)
#pylint: enable=wrong-import-position


def main():
  parser = argparse.ArgumentParser(
      description='Deletes local branches identical (no diff) to effective parent. Updates children.',
      formatter_class=argparse.RawTextHelpFormatter)
  parser.add_argument(
      '--dry-run',
      action='store_true',
      help="Show actions without making changes.")
  args = parser.parse_args()

  if args.dry_run:
    print("--- DRY RUN MODE ---")

  # --- Phase 1: Check and Map ---
  print("Analyzing branch structure...")
  sorted_branches: List[str] = []
  parent_graph: Dict[str, Optional[str]] = {}
  try:
    sorted_branches, parent_graph = topological_sort_branches()
  except ValueError as e:
    print(f"Error: {e}", file=sys.stderr)
    sys.exit(1)
  except Exception as e:
    print(f"Dependency analysis error: {e}", file=sys.stderr)
    sys.exit(1)

  if not sorted_branches:
    print("No branches with parent configurations found.")
    sys.exit(0)

  remap: Dict[str, str] = {}  # {pruned_branch: effective_parent_it_matched}
  branches_to_prune: Set[str] = set()
  all_local_branches = set(get_all_branches())

  print("Checking branches against effective parents...")
  for branch in sorted_branches:
    original_parent = parent_graph.get(branch)
    if original_parent is None:
      continue
    effective_parent = remap.get(original_parent, original_parent)
    if not effective_parent:
      continue
    if effective_parent not in all_local_branches and effective_parent not in MAINLINE_BRANCHES:
      continue

    diff_cmd = ['diff', '--quiet', effective_parent, branch]
    try:
      diff_result = run_git_command(diff_cmd, check=False)
      is_identical = (diff_result.returncode == 0)
    except Exception as e:
      print(
          f"Warning: Error diffing '{effective_parent}'..'{branch}': {e}. Skipping.",
          file=sys.stderr)
      continue

    if is_identical:
      print(
          f"- Found: '{branch}' identical to effective parent '{effective_parent}'. Marking for prune."
      )
      branches_to_prune.add(branch)
      remap[branch] = effective_parent

  if not branches_to_prune:
    print("\nNo branches found to be pruned.")
    sys.exit(0)

  # --- Phase 2: Perform Actions ---
  print("\n--- Actions ---")
  if args.dry_run:
    print("Dry Run - Would perform:")
    for branch_to_prune in sorted(list(branches_to_prune)):
      final_parent = remap.get(branch_to_prune, "???")
      print(
          f" - Delete branch '{branch_to_prune}' (identical to '{final_parent}')"
      )
      children = get_branch_children(branch_to_prune, list(all_local_branches))
      children_to_reparent = [c for c in children if c not in branches_to_prune]
      if children_to_reparent:
        print(
            f"   - Re-parent children ({', '.join(children_to_reparent)}) to '{final_parent}'"
        )
  else:
    print("Performing re-parenting and deletions...")
    current_checked_out_branch = get_current_branch()
    all_local_branches_list = list(all_local_branches)
    reparent_errors = False
    processed_children = set()

    print("Updating parent config for children...")
    for branch_to_prune in branches_to_prune:
      new_parent = remap.get(branch_to_prune)
      if not new_parent:
        print(
            f"Error: No remap parent for '{branch_to_prune}'. Skipping children.",
            file=sys.stderr)
        reparent_errors = True
        continue
      children = get_branch_children(branch_to_prune, all_local_branches_list)
      for child in children:
        if child not in branches_to_prune and child not in processed_children:
          try:
            print(
                f" - Setting parent of '{child}' to '{new_parent}' (was '{branch_to_prune}')"
            )
            run_git_command(['config', f'branch.{child}.parent', new_parent])
            processed_children.add(child)
          except Exception as e:
            print(f"Error updating config for '{child}': {e}", file=sys.stderr)
            reparent_errors = True

    print("Deleting branches...")
    delete_errors = False
    for branch_to_prune in sorted(list(branches_to_prune)):
      if branch_to_prune == current_checked_out_branch:
        print(f"Skipping delete of '{branch_to_prune}' (checked out).")
        continue
      print(f" - Deleting branch '{branch_to_prune}'...")
      try:
        # Use -D for force delete, as branch might not appear merged
        run_git_command(['branch', '-D', branch_to_prune])
      except SystemExit:
        delete_errors = True  # Report error but continue deleting others
      except Exception as e:
        print(
            f"Unexpected error deleting '{branch_to_prune}': {e}",
            file=sys.stderr)
        delete_errors = True

    if reparent_errors or delete_errors:
      print("\nWarning: Errors occurred.", file=sys.stderr)
      sys.exit(1)

  print(f"\n--- Pruning process finished ---")
  print(
      f"Branches {'would have been' if args.dry_run else 'were'} pruned: {len(branches_to_prune)}"
  )


if __name__ == "__main__":
  main()
