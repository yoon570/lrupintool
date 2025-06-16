#!/bin/bash

unclfreq=(1000 10000 100000 1000000 10000000)
clfreq=(1000 10000 100000 1000000 10000000)

for uf in "${unclfreq[@]}"; do
    for cf in "${clfreq[@]}"; do
        if (( uf >= cf )); then
            echo -e "\nCFREQ:$cf, UFREQ:$uf" >> mblast20.log

            /home/cs0006258/Desktop/pintool/pin \
                -t /home/cs0006258/Desktop/pintool/source/tools/lru_policy/obj-intel64/lru_policy.so \
                -unclsize 2000 -clsize 4000 \
                -unclfreq "$uf" -clfreq "$cf" -exfreq "$uf" \
                -- BENCHMARK_HERE
                >> /home/cs0006258/Desktop/pintool/experim/mblast20.log 2>&1
        fi
    done
done

