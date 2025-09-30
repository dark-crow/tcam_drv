#!/bin/bash

echo "----------------------------------------------------"
echo "usage: $0 tcam-vdo/tcam-raw"
echo "       $#"
echo "----------------------------------------------------"
echo ""

tcam_vdo_drv=null;
tcam_raw_drv=null;

#  check Raspberry Pi 5
model=$(tr -d '\0' </proc/device-tree/model)
if [[ $model == *"Raspberry Pi 5"* ]]; then
    echo "This is a $model."
elif [[ $model == *"Raspberry Pi Compute Module 5"* ]]; then
    echo "This is a $model."
else
    echo "This is not a Raspberry Pi 5."
    echo "This is a $model."
	exit 0;
fi
echo ""

# check config.txt location
CONFIG_FILE=""
if [ -f /boot/firmware/config.txt ]; then
    CONFIG_FILE="/boot/firmware/config.txt"
else
	CONFIG_FILE="/boot/config.txt"
fi

echo "CONFIG_FILE: $CONFIG_FILE"
echo ""

valid_drivers=("tcam-vdo" "tcam-raw")

if [ $# -eq 0 ]; then
    tcam_vdo_drv="tcam-vdo";
    tcam_raw_drv="tcam-raw";
elif [ $# -eq 1 ]; then
    if [ "$1" == ${valid_drivers[0]} ]; then
        tcam_vdo_drv="tcam-vdo";
    elif [ "$1" == ${valid_drivers[1]} ]; then
        tcam_raw_drv="tcam-raw";
    else
        echo "Please provide a correct camera module name!"
        exit 0;
    fi      
else
    echo "Invalid parameter!"
    exit 0
fi

write_i2c_vc_to_config()
{
    awk "BEGIN{ count=0 }\
    {\
        if(\$1 == \"dtparam=i2c_vc=on\"){\
            count++;\
        }\
    }END{\
        if(count <= 0){\
            system(\"sudo sh -c 'echo dtparam=i2c_vc=on >> ${CONFIG_FILE}'\");\
        }\
    }" "${CONFIG_FILE}"
}

register_tcam_driver()
{
    local tcam_name="$1"

    echo "--------------------------------------"
    echo "Add dtoverlay=$tcam_name to ${CONFIG_FILE} "
	awk "BEGIN{ count=0 } \
	 { \
		 if(\$1 == \"dtoverlay=${tcam_name}\"){ \
			 count++; \
		 } \
	 } \
	 END{ \
		 if(count <= 0){ \
			 system(\"sudo sh -c 'echo dtoverlay=${tcam_name},cam1 >> ${CONFIG_FILE}'\"); \
		 } \
	 }" "${CONFIG_FILE}"

    sudo install -p -m 644 ./drv_bin/$(uname -r)/$tcam_name.ko  /lib/modules/$(uname -r)/kernel/drivers/media/i2c/
    sudo install -p -m 644 ./drv_bin/$(uname -r)/$tcam_name.dtbo /boot/overlays/

    echo "Installing the $tcam_name.ko driver"
    echo "--------------------------------------"
}

# load i2c-dev kernel module
echo "--------------------------------------"
echo "Enable i2c adapter... pls use i2c-11 for cam1, i2c-10 for cam0"
echo "--------------------------------------"
echo ""
sudo modprobe i2c-dev

# add dtparam=i2c_vc=on to ${CONFIG_FILE}
write_i2c_vc_to_config;

if [ "$tcam_vdo_drv" == "tcam-vdo" ]; then
    register_tcam_driver $tcam_vdo_drv
fi
echo ""

if [ "$tcam_raw_drv" == "tcam-raw" ]; then
    register_tcam_driver $tcam_raw_drv
fi
echo ""
sudo /sbin/depmod -a $(uname -r)

echo "reboot now?(y/n):"
read USER_INPUT
case $USER_INPUT in
'y'|'Y')
    echo "reboot"
    sudo reboot
;;
*)
    echo "cancel"
;;
esac

        
