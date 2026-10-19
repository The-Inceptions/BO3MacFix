#!/bin/sh
# keys.sh CODE[xN] ...: presses each macOS virtual key code (N times) through the com.bo3macfix.test.key hook
# Return 36, Esc 53, Left 123, Right 124, Down 125, Up 126, F10 109, Space 49
for arg in "$@"; do
    code=${arg%x*}; n=1; case $arg in *x*) n=${arg#*x};; esac
    i=0; while [ $i -lt $n ]; do
        notifyutil -s com.bo3macfix.test.key "$code" -p com.bo3macfix.test.key; sleep 0.25; i=$((i + 1))
    done
done
