#!/bin/bash

unclfreq=(100 1000 10000 100000 1000000)
exfreq=(100 1000 10000 100000 1000000)

for ef in "${exfreq[@]}"; do
    for uf in "${unclfreq[@]}"; do
            echo -e "\nEFREQ:$ef, UFREQ:$uf" >> mbmt.log
            /home/cs0006258/Desktop/pintool/pin \
            -t /home/cs0006258/Desktop/pintool/source/tools/lru_policy/obj-intel64/lru_policy.so \
            -unclsize 2000 -clsize 4000 \
            -unclfreq "$uf" -clfreq 1 -exfreq "$ef" \
            -- /home/cs0006258/Desktop/pintool/source/tools/lru_policy/mb_mt 10000 1000000 \
            >> mbmt.log 2>&1
    done
done
