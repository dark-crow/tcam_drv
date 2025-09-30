#!/bin/bash

INSTALL_DIR="install/drv_bin"
DRVSRC_DIR="drv_src"
DTSSRC_DIR="dts"


# 현재 라즈베리파이 커널 버전을 설정
RPIVER=$(uname -r)

echo "Current RPI version: $RPIVER"

# 해당 커널 버전에 맞는 디렉토리를 생성
if [[ ! -e "$INSTALL_DIR/$RPIVER" ]]; then
    echo "Creating directory for kernel version $RPIVER"
    mkdir -p $INSTALL_DIR/$RPIVER
fi

FILE_LISTS=("tcam-vdo.ko" "tcam-raw.ko" "tcam-vdo.dtbo" "tcam-raw.dtbo")

# echo "FILE_LISTS: ${FILE_LISTS[0]}"
# echo "FILE_LISTS: ${FILE_LISTS[1]}"
# echo "FILE_LISTS: ${FILE_LISTS[2]}"
# echo "FILE_LISTS: ${FILE_LISTS[3]}"

# echo "$INSTALL_DIR/$RPIVER/${FILE_LISTS[0]}"
# echo "$INSTALL_DIR/$RPIVER/${FILE_LISTS[1]}"
# echo "$INSTALL_DIR/$RPIVER/${FILE_LISTS[2]}"
# echo "$INSTALL_DIR/$RPIVER/${FILE_LISTS[3]}"

# 기존 파일들을 삭제
for file in "${FILE_LISTS[@]}"; do
    if [[ -e "$INSTALL_DIR/$RPIVER/$file" ]]; then
        echo "$file exists and will be removed."
        rm -rf $INSTALL_DIR/$RPIVER/$file
    fi
done
echo "Old driver files removed."

# 드라이버 컴파일
DRVBUILDLOG=$(cd $DRVSRC_DIR && make clean && make 2>&1)
DRVBUILDRES=$?

if [[ $DRVBUILDRES -ne 0 ]]; then
    echo "Driver compile error"
    echo "$DRVBUILDLOG"
    exit 1
fi

echo "Driver compile result OK!"

# DTS 컴파일
DTSBUILDLOG=$(cd $DTSSRC_DIR && ./build_dts.sh 2>&1)
DTSBUILDRES=$?

if [[ $DTSBUILDRES -ne 0 ]]; then
    echo "DTS compile error"
    echo "$DTSBUILDLOG"
    exit 1
fi

echo "DTS compile result OK!"


# 파일 복사
cp $DRVSRC_DIR/*.ko $INSTALL_DIR/$RPIVER/
cp $DTSSRC_DIR/*.dtbo $INSTALL_DIR/$RPIVER/

echo "Driver files copied to $INSTALL_DIR/$RPIVER/"
echo "--------------------------------------"
echo "Driver build and installation files are ready."
echo "You can now run the install script to install the drivers."
echo "--------------------------------------"
echo "You can now run the uninstall script to uninstall the drivers."
echo "--------------------------------------"
echo "File list:"
for file in "${FILE_LISTS[@]}"; do
    if [[ -e "$INSTALL_DIR/$RPIVER/$file" ]]; then
        echo "$file"
    else
        echo "$file is missing!"
    fi
done
echo "--------------------------------------"
echo "To install the driver, run:"
echo "  sudo bash install/install_driver.sh <camera_module_name>"
echo "To uninstall the driver, run:"
echo "  sudo bash install/uninstall_driver.sh <tcam-vdo/tcam-raw>"
echo "--------------------------------------"
exit 0 