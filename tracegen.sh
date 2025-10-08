#!/bin/bash

./pin -t source/tools/tracegen/obj-intel64/tracegen.so -tracefile mb_spatial_rotating_write.trace -outfile mb_spatial_rotating_write.out -- microbenchmarks/mb_spatial_rotating_write 20000 1000000 5
./pin -t source/tools/tracegen/obj-intel64/tracegen.so -tracefile mb_spatial_rotating.trace -outfile mb_spatial_rotating.out -- microbenchmarks/mb_spatial_rotating 20000 1000000 5
./pin -t source/tools/tracegen/obj-intel64/tracegen.so -tracefile mb_spatial_sequential.trace -outfile mb_spatial_sequential.out -- microbenchmarks/mb_spatial_sequential 20000 1000000
./pin -t source/tools/tracegen/obj-intel64/tracegen.so -tracefile mb_spatial_sequential_write.trace -outfile mb_spatial_sequential_write.out -- microbenchmarks/mb_spatial_write 20000 1000000 
./pin -t source/tools/tracegen/obj-intel64/tracegen.so -tracefile mb_temporal.trace -outfile mb_temporal.out -- microbenchmarks/mb_temporal 20000 1000000 0.8 4000
./pin -t source/tools/tracegen/obj-intel64/tracegen.so -tracefile mb_temporal_write.trace -outfile mb_temporal_write.out -- microbenchmarks/mb_temporal_write 20000 1000000 0.8 4000
./pin -t source/tools/tracegen/obj-intel64/tracegen.so -tracefile mb_uniform_write.trace -outfile mb_uniform_write.out -- microbenchmarks/mb_uniform_write -rss 20000 -total_iters 1000000
./pin -t source/tools/tracegen/obj-intel64/tracegen.so -tracefile mb_uniform.trace -outfile mb_uniform.out -- microbenchmarks/mb_uniform -rss 20000 -total_iters 1000000
