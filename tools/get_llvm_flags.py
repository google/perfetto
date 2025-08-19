import sys
import subprocess
import os


def run_llvm_config(args):
  """
  Runs llvm-config with the given arguments and returns its stripped stdout.
  Exits the script if llvm-config is not found or returns an error.
  """
  try:
    return subprocess.check_output(
        ["llvm-config"] + args,
        text=True,  # Decode output as text (Python 3)
    ).strip()
  except (subprocess.CalledProcessError, FileNotFoundError) as e:
    sys.stderr.write(f"Error: Failed to run 'llvm-config {' '.join(args)}'.\n")
    if isinstance(e, subprocess.CalledProcessError):
      sys.stderr.write(f"STDOUT: {e.stdout}\n")
      sys.stderr.write(f"STDERR: {e.stderr}\n")
    else:  # FileNotFoundError
      sys.stderr.write(
          "Ensure 'llvm-config' is installed and in your system's PATH.\n")
    sys.exit(1)


def format_gn_list(items):
  """Formats a Python list into a GN-compatible list string, removing duplicates and sorting."""
  if not items:
    return "[]"
  return "[\n" + ",\n".join(
      f'  "{item}"' for item in sorted(list(set(items)))) + "\n]"


def main():
  llvm_include_dir = run_llvm_config(["--includedir"])
  cxxflags_raw = run_llvm_config(["--cxxflags"])

  defines_list = []
  cflags_list = []

  for flag in cxxflags_raw.split():
    if flag.startswith('-D'):
      defines_list.append(flag[2:])
    elif not flag.startswith('-I'):
      cflags_list.append(flag)

  cflags_list.insert(0, f'-isystem{llvm_include_dir}')

  # Get ALL flags and library names needed for SHARED linking
  ldflags_and_libs_raw = run_llvm_config(
      ["--ldflags", "--libs", "--link-shared", "symbolize"])

  ldflags_list = []
  libs_list = []

  for part in ldflags_and_libs_raw.split():
    if part.startswith('-L'):
      # Add library search paths (e.g., "-L/usr/lib/llvm-19/lib")
      ldflags_list.append(part)
    elif part.startswith('-l'):
      # Add library names (e.g., "LLVM-19", "z")
      libs_list.append(part[2:])
    else:
      # Add other miscellaneous flags
      ldflags_list.append(part)

  # Print output in GN-compatible format
  print(f"defines = {format_gn_list(defines_list)}")
  print(f"cflags = {format_gn_list(cflags_list)}")
  print(f"libs = {format_gn_list(libs_list)}")
  print(f"ldflags = {format_gn_list(ldflags_list)}")
  return 0


if __name__ == '__main__':
  sys.exit(main())
