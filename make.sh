#!/bin/bash
set -e
gcc -O3 ipts.c -lpng -lm -lSDL2 -lSDL2_ttf -o ipts
#./ipts
