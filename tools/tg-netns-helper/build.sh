#!/bin/sh
# Build tg-netns-helper.
#
# Run on Linux (or in a Linux container). The output binary must be
# installed setuid root:
#   sudo install -o root -m 4755 tg-netns-helper /usr/local/sbin/
set -e
cd "$(dirname "$0")"
${CC:-gcc} -O2 -Wall -Wextra -Wpedantic -o tg-netns-helper main.c
echo "Built ./tg-netns-helper. Install with:"
echo "  sudo install -o root -m 4755 tg-netns-helper /usr/local/sbin/"
