#!/bin/bash

/home/yoonl18/lrupintool/pin \
  -t obj-intel64/lru_policy.so \
  -unclsize 2000 -clsize 4000 -clfreq 1 -unclfreq 200 -exfreq 6000 -repival 100000000 \
  -- /home/yoonl18/lrupintool/source/tools/lru_policy/mb_sweep \
  -rss 10000 -total_iters 1000000 \
  > /home/yoonl18/lrupintool/source/tools/lru_policy/test.log 2>&1

