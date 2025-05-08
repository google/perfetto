# PerfettoSQL Style Guide

_This page provides a suggested style guide for writing PerfettoSQL and is in
use inside the PerfettoSQL standard library in the trace processor. It also
provides guidance on the autoformatter._

## Rules

1. Keep lines below 80 characters long
2. Function names, macro names and table/view names should all be lower snake
   case.
3. SQL keywords should all be upper case
4. When line-breaking SQL expressions, place the joining keyword (AND/OR) at the
   _start of the next line_ rather than the _end of the previous line_.

## Autoformatter

PerfettoSQL comes with an auto-formatter as implemented by `tools/format_sql.py`
Python script. This is a simple script which can be run over any set of files or
directories and automatically formats the code to adhere to the above rules.

This script is _required_ to be run when making contributions to the standard
library. It's automatically executed as part of running `tools/gen_all` which is
part of the standard development workflow in Perfetto. Presubmit will check to
make sure you've done this.

The autoformatter is not perfect by any means and does often have subtle edge
cases:
1. It doesn't handle comments in the middle of complex expressions. Prefer
placing comments at the start of these expressions instead of in the middle.
2. It doesn't format macro invocations as they have very complex semantics
in many cases.
