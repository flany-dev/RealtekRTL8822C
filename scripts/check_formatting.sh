#!/bin/sh
set -eu

list_file="$(mktemp -t rtl8822c-format.XXXXXX)"
trap 'rm -f "$list_file"' EXIT HUP INT TERM

find . \( -path './.git' -o -path './build' \) -prune -o \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o \
    -name '*.h' -o -name '*.md' -o -name '*.sh' -o -name '*.py' -o \
    -name '*.yml' \) -print0 > "$list_file"

if xargs -0 grep -nE '[[:blank:]]$' < "$list_file"; then
    echo "format check failed: trailing whitespace" >&2
    exit 1
fi

if xargs -0 grep -n "$(printf '\r')" < "$list_file"; then
    echo "format check failed: CRLF input" >&2
    exit 1
fi

echo "Formatting check passed"
