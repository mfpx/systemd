#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
set -o pipefail

for i in *.h */*.h; do
    curl --fail "https://raw.githubusercontent.com/torvalds/linux/master/include/uapi/linux/$i" -o "$i"

    sed -r -i -e 's/__user //g' -e '/^#include <linux\/compiler(_types)?.h>/ d' "$i"
done
