#!/bin/bash
cd "$(dirname "$0")/.."
grep -n 'java_static_field\|POWER_SERVICE\|VIBRATOR\|Build\$VERSION' src/androidfw.c | head -40
grep -n 'java_static_field\|java_field\|java_method' src/fakejni.h
