#! /bin/sh
SYNAXIS_VERSION="2.3.4"
echo "Extracting src/version.c ..."
if [ -d ../.git ] && command -v git >/dev/null 2>&1; then
    git_desc=$(git describe --tags --always 2>/dev/null)
    if [ -n "$git_desc" ]; then
        cvs_version="Synaxis-${git_desc}"
    else
        git_hash=$(git rev-parse --short HEAD 2>/dev/null)
        [ -n "$git_hash" ] && cvs_version="Synaxis-${SYNAXIS_VERSION}-g${git_hash}" || cvs_version="Synaxis-${SYNAXIS_VERSION}"
    fi
else
    cvs_version="Synaxis-${SYNAXIS_VERSION}"
fi
echo "  version: $cvs_version"
/bin/cat >version.c <<!SUB!THIS!
/* version.c - Synaxis IRC Services
 * Copyright 2024-2026 Cathexis Development */
#include "version.h"
const char *cvs_version = "$cvs_version";
!SUB!THIS!
