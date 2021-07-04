#!/bin/bash

#Find the absolute path where we are called from
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

#Set the library path for the dynamic linker to find it
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$SCRIPT_DIR

#Preload the library - Our malloc has the highest priority
LD_PRELOAD=$SCRIPT_DIR/libxmalloc.so

#Run test_alloc for each case
for ((c=0; c < 9; c++))
do
  ./test_alloc $c
done

