#!/bin/bash
set -e
gcc ipts.c -lpng -lm -lSDL2 -lSDL2_ttf -o ipts
./ipts
