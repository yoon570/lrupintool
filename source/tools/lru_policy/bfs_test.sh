#!/bin/bash
exec setarch "$(uname -m)" -R /home/yoonl18/graphBIG/benchmark/bfs_nopin "$@"
