#!/usr/bin/env python3
"""
Experimental changelog and release notes generation tool for Perfetto.

This script automates the process of generating release announcements by:
1. Extracting commit logs since the last release
2. Guiding the user through AI-assisted CHANGELOG updates
3. Generating a complete release announcement

Usage: python tools/release/gen_release_notes.py [--since-tag TAG] [--output-dir DIR]
"""

import argparse
import subprocess
import sys
from pathlib import Path


def get_git_commits_since_tag(since_tag=None):
  """Extract git commits since the specified tag or auto-detect the latest release tag."""

  if not since_tag:
    try:
      result = subprocess.run(['git', 'tag', '--sort=-version:refname'],
                              capture_output=True,
                              text=True,
                              check=True)
      tags = result.stdout.strip().split('\n')
      release_tags = [tag for tag in tags if tag.startswith('v') and '.' in tag]
      if not release_tags:
        print("No release tags found. Please specify --since-tag manually.")
        sys.exit(1)
      since_tag = release_tags[0]
      print(f"Auto-detected latest release tag: {since_tag}")
    except subprocess.CalledProcessError:
      print("Failed to get git tags. Make sure you're in a git repository.")
      sys.exit(1)

  try:
    result = subprocess.run(['git', 'log', '--oneline', f'{since_tag}..HEAD'],
                            capture_output=True,
                            text=True,
                            check=True)
    commits = result.stdout.strip()
    if not commits:
      print(f"No commits found since {since_tag}")
      sys.exit(1)
    return since_tag, commits
  except subprocess.CalledProcessError:
    print(f"Failed to get commits since {since_tag}. Make sure the tag exists.")
    sys.exit(1)


def detect_current_version():
  """Detect current version tag or prompt user for it."""
  try:
    result = subprocess.run(
        ['git', 'describe', '--tags', '--exact-match', 'HEAD'],
        capture_output=True,
        text=True,
        check=True)
    current_version = result.stdout.strip()
    print(f"Current version detected: {current_version}")
    return current_version
  except subprocess.CalledProcessError:
    return input("Enter the current version tag (e.g., v52.0): ").strip()


def write_commits_to_file(commits, output_file):
  """Write the commit log to a file."""
  with open(output_file, 'w') as f:
    f.write(commits)
  print(f"Commits written to: {output_file}")


def generate_changelog_update_prompt(commits_file):
  """Generate the prompt for AI to update the changelog."""

  prompt = f"""# Changelog Update Prompt for Perfetto Release

Please analyze the commits and current CHANGELOG to generate an updated changelog that includes all unreleased changes.

## Input Files:
- Commit log: {commits_file}
- Current CHANGELOG: Use the Read tool to read the CHANGELOG file directly

## Instructions:

1. **Read both files carefully** - The commit log shows all changes since the last release, the CHANGELOG shows what's already documented

2. **Identify unreleased changes** that should be added to the CHANGELOG:
   - Look for commits that represent user-facing changes
   - Focus on new features, bug fixes, performance improvements, breaking changes
   - Skip internal refactoring, CI changes, and minor code cleanup unless they have user impact
   - Group related commits together logically

3. **Follow Perfetto's CHANGELOG format** (based on existing structure):
   - Categories: "Tracing service and probes:", "SQL Standard library:", "Trace Processor:", "UI:", "SDK:", "Tools:", "Docs:"
   - Use bullet points with asterisks (*)
   - Each entry should be concise but descriptive
   - Include technical details but keep accessible to users
   - Use consistent indentation and formatting

4. **Add missing entries** from the unreleased commits:
   - Compare commits against existing CHANGELOG entries
   - Add any significant changes that are missing
   - Maintain logical grouping within categories
   - Ensure no duplicate entries

5. **Output format**:
   - Provide the complete updated CHANGELOG content
   - Keep the existing "Unreleased:" section and enhance it with missing changes
   - Preserve all existing content below the unreleased section
   - Follow the exact formatting style seen in the current CHANGELOG

## Important Notes:
- Focus on changes that affect end users, not internal development
- Maintain the established writing style and technical level
- Use the same terminology and phrasing patterns as existing entries
- Group related commits into single changelog entries where appropriate
- Preserve existing formatting and structure exactly

## Expected Output:
Use the Edit tool to update the CHANGELOG file directly at: CHANGELOG

Update the existing "Unreleased:" section with any missing changes from the commits, maintaining the exact same format and structure as the existing file.
"""

  return prompt


def generate_release_notes_prompt(commits_file, since_tag, current_version):
  """Generate the prompt for AI release notes generation."""

  github_compare_url = f"https://github.com/google/perfetto/compare/{since_tag}...{current_version}"
  github_changelog_url = f"https://github.com/google/perfetto/blob/{current_version}/CHANGELOG"

  prompt = f"""You are a technical release notes writer for Perfetto, a performance analysis and tracing platform. Your task is to transform raw changelog entries into engaging, well-structured release notes.

## Example Input (Raw Changelog):
```
Unreleased:
  Tracing service and probes:
    * Added support for exclusive single-tenant features in ftrace data source.
      These features can only be used by a single tracing session.
    * Added support for tracing_cpumask, tracefs options and ftrace filtering
      by TID as exclusive features.
    * Added support for polling Adreno GPU frequency in SysStatsDataSource.
    * Deprecated: "resolve_process_fds" option in "linux.process_stats" data
      source. Asynchronous scraping is too unreliable and there are no known
      maintainers or users.
  SQL Standard library:
    * Added new power analysis capabilities with expanded Wattson device support
      and IRQ power attribution for more accurate power profiling.
    * Added suspend-aware CPU utilization metrics for better analysis of
      power-managed systems.
    * Added anr_type column to android_anrs table for improved ANR debugging.
    * Added `android.bitmaps` module with timeseries information about bitmap
      usage in Android.
  Trace Processor:
    * Significantly improved performance with optimized data structures and
      MurmurHash implementation, resulting in faster trace loading and query
      execution.
    * Added slice_self_dur table for more efficient self-duration calculations.
    * Added regexp_extract function for improved string processing in queries.
    * Added support for `sibling_merge_behavior` and `sibling_merge_key` in
      `TrackDescriptor` for TrackEvent, allowing for finer-grained control over
      how tracks are merged.
  UI:
    * Added comprehensive dark mode support with theme-aware colors throughout
      the interface.
    * Introduced bulk track settings management allowing users to configure
      multiple tracks simultaneously.
    * Added startup commands feature for automated trace analysis workflows.
    * Fixed numerous crashes and performance issues, including flamegraph
      crashes and selection performance problems.
```

## Example Output (Polished Release Notes):

# Perfetto v52.0 Release Notes

We're excited to announce Perfetto v52.0, packed with significant improvements to the user experience, performance analysis capabilities, and recording infrastructure.

## 🌙 Comprehensive Dark Mode Support

The Perfetto UI now features a complete dark mode implementation with theme-aware colors throughout the entire interface. This long-requested feature makes it comfortable to analyze traces in low-light environments and provides a modern, professional appearance that many developers prefer.

## 🎛️ Advanced TrackEvent Control & Visualization

SDK users and developers converting external traces to Perfetto format now have fine-grained control over track display and merging behavior. The new `sibling_merge_behavior` and `sibling_merge_key` options in `TrackDescriptor` allow you to:

- Force tracks with the same name to be displayed separately
- Merge tracks with different names into a single UI track
- Override the default name-based merging logic

Additionally, counter tracks can now share Y-axis ranges using the `y_axis_share_key` in `CounterDescriptor`, making it easier to compare related metrics with the same units.

Learn more about [converting custom data to Perfetto format](https://perfetto.dev/docs/getting-started/converting) and [advanced synthetic track event configuration](https://perfetto.dev/docs/reference/synthetic-track-event).

## ⚡ Trace Processor Performance & New Features

This release delivers significant performance improvements and new analysis capabilities:

- **Faster trace loading**: Optimized data structures and MurmurHash implementation result in noticeably faster trace loading and query execution
- **New analysis tools**: The `slice_self_dur` table provides efficient self-duration calculations, while the `regexp_extract` function enhances string processing in SQL queries

## 📱 Enhanced Android Analysis Capabilities

Android developers gain powerful new debugging and performance analysis tools:

- **Better ANR debugging**: The `anr_type` column in the android_anrs table provides more detailed ANR classification
- **Bitmap tracking**: New `android.bitmaps` module offers timeseries information about bitmap usage

## 🔧 Additional Improvements

- Fixed numerous crashes and performance issues throughout the UI
- Enhanced support for polling Adreno GPU frequency data

---

For complete details, see the [changelog]({github_changelog_url}) or [view all changes on GitHub]({github_compare_url}). Download Perfetto v52.0 from our [releases page](https://github.com/google/perfetto/releases), get started at [docs.perfetto.dev](https://docs.perfetto.dev), or try the UI directly at [ui.perfetto.dev](https://ui.perfetto.dev).

## Instructions:
1. Follow the exact structure, tone, and formatting of the example output above
2. Transform technical changelog entries into user-focused benefits
3. Group related features into thematic sections with emoji headers
4. Use enthusiastic but professional language
5. Include code formatting for technical terms (backticks)
6. Add "Learn more" placeholder links for complex features
7. Bold key feature names in bullet points
8. End with the standard closing paragraph (adapt version number)
9. Focus on what users can DO with the new features, not just what was added

## Input Files:
- Commit log: {commits_file}
- Updated CHANGELOG: Use the Read tool to read the CHANGELOG file directly

## Auto-Generated Links (use these exact URLs):
- **Changelog**: {github_changelog_url}
- **Full changes**: {github_compare_url}
- **Documentation**: https://perfetto.dev/docs/ (primary documentation site)
- **UI only**: https://ui.perfetto.dev
- **Releases**: https://github.com/google/perfetto/releases

Transform the provided changelog into release notes matching this style and structure exactly."""

  return prompt


def main():
  parser = argparse.ArgumentParser(
      description='Generate Perfetto release notes')
  parser.add_argument(
      '--since-tag',
      help='Git tag to start from (auto-detects latest if not specified)')
  parser.add_argument(
      '--output-dir',
      default='/tmp',
      help='Directory to write output files (default: /tmp)')

  args = parser.parse_args()

  if not Path('.git').exists():
    print("This script must be run from the root of a git repository.")
    sys.exit(1)

  output_dir = Path(args.output_dir)
  output_dir.mkdir(exist_ok=True)

  print("=== Perfetto Release Notes Generator ===\n")

  print("Step 1: Extracting commits since last release...")
  since_tag, commits = get_git_commits_since_tag(args.since_tag)

  current_version = detect_current_version()

  commits_file = output_dir / f'commits_since_{since_tag}.txt'
  write_commits_to_file(commits, commits_file)

  print("\nStep 2: Generating changelog update prompt...")
  changelog_update_prompt = generate_changelog_update_prompt(commits_file)
  changelog_update_prompt_file = output_dir / 'changelog_update_prompt.txt'

  with open(changelog_update_prompt_file, 'w') as f:
    f.write(changelog_update_prompt)

  print(f"Changelog update prompt written to: {changelog_update_prompt_file}")
  print("\n" + "=" * 60)
  print("NEXT STEP: Changelog Update")
  print("=" * 60)
  print(f"1. Copy the prompt from: {changelog_update_prompt_file}")
  print(f"2. Provide it to an AI along with the commit file: {commits_file}")
  print("3. The AI will read CHANGELOG directly and update it in place")
  print("4. Return here and press Enter when the CHANGELOG has been updated...")

  input("\nPress Enter when the CHANGELOG has been updated...")

  print("\nStep 3: Generating release notes prompt...")
  release_notes_prompt = generate_release_notes_prompt(commits_file, since_tag,
                                                       current_version)
  release_notes_prompt_file = output_dir / 'release_notes_prompt.txt'

  with open(release_notes_prompt_file, 'w') as f:
    f.write(release_notes_prompt)

  print(f"Release notes prompt written to: {release_notes_prompt_file}")
  print("\n" + "=" * 60)
  print("FINAL STEP: Generate Release Notes")
  print("=" * 60)
  print(f"1. Copy the prompt from: {release_notes_prompt_file}")
  print(
      f"2. In a NEW AI conversation, provide the prompt along with: {commits_file}"
  )
  print("3. The AI will generate publication-ready release notes!")
  print("\n" + "=" * 60)
  print("Release notes generation ready!")
  print("=" * 60)


if __name__ == '__main__':
  main()
