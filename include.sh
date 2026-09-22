#!/bin/bash

## BASH script for including module mod-azeroth-friend
# Called from apps/docker/docker-build-dev.sh or build scripts

MOD_AZEROTH_FRIEND_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
source "$MOD_AZEROTH_FRIEND_ROOT/conf/mod_azeroth_friend.conf" 2>/dev/null || true
