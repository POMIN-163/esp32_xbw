#!/bin/bash

. ${IDF_PATH}/add_path.sh
esptool.py --chip esp32 --port "/dev/ttyS22" --baud $((230400*4)) write_flash -fs 4MB 0x100000 "build/nesemu.bin"
