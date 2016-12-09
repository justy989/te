#!/bin/bash

clang -o te main.c -g -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Werror -std=c11 -lncurses -lpthread -lutil
