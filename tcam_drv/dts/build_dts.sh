#!/bin/bash

# rm -f tcam-vdo.dtbo
# rm -f tcam-raw.dtbo

dtc -@ -Hepapr -I dts -O dtb -o tcam-vdo.dtbo tcam-vdo-overlay.dts
DTSRES=$?
if [[ $DTSRES -ne 0 ]]; then
    echo "DTS compile error"
    exit 1
fi


dtc -@ -Hepapr -I dts -O dtb -o tcam-raw.dtbo tcam-raw-overlay.dts
DTSRES=$?
if [[ $DTSRES -ne 0 ]]; then
    echo "DTS compile error"
    exit 1
fi

exit 0