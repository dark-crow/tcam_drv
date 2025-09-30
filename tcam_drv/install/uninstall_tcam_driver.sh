#!/bin/bash

echo "----------------------------------------------------"
echo "usage: $0 tcam-vdo/tcam-raw"
echo "       $#"
echo "----------------------------------------------------"
echo ""

tcam_vdo_drv=null;
tcam_raw_drv=null;

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

unregister_tcam_driver()
{
    local tcam_name="$1"

    echo "--------------------------------------"
    echo "Delete dtoverlay=$tcam_name to ${CONFIG_FILE} "

    #sudo sed "s/^dtoverlay=${tcam_name}/#dtoverlay=${tcam_name}/g" -i ${CONFIG_FILE}
    sudo sed "/^dtoverlay=${tcam_name}/d" -i ${CONFIG_FILE}

    sudo rm /lib/modules/$(uname -r)/kernel/drivers/media/i2c/$tcam_name.ko
    sudo rm /boot/overlays/$tcam_name.dtbo
  
    echo "Uninstalling the $tcam_name.ko driver"
    echo "--------------------------------------"
}

if [ "$tcam_vdo_drv" == "tcam-vdo" ]; then
    unregister_tcam_driver $tcam_vdo_drv
fi
echo ""

if [ "$tcam_raw_drv" == "tcam-raw" ]; then
    unregister_tcam_driver $tcam_raw_drv
fi
echo ""

sudo /sbin/depmod -a $(uname -r)
#sudo sed 's/^dtoverlay=$driver_name/#dtoverlay=$driver_name/g' -i ${CONFIG_FILE}


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

