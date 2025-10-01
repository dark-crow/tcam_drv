#!/bin/bash

# I2C bus numbers for camera modules
I2CBUS_CAM1=11
I2CBUS_CAM0=10

#default params of Thermal camera video driver
TCAMVDO_WIDTH=384
TCAMVDO_HEIGHT=288
TCAMVDO_MEDIA_FMT=YUYV8_1X16
TCAMVDO_PIXEL_FMT=YUYV

#default params of Thermal camera raw driver
TCAMRAW_WIDTH=384
TCAMRAW_HEIGHT=289
TCAMRAW_MEDIA_FMT=UYVY8_1X16
TCAMRAW_PIXEL_FMT=UYVY

g_tcam_vdo_drv=null;
g_tcam_raw_drv=null;

g_media_device=null
g_video_device=null
g_video_subdevice=null

g_unpacked_supported=0

# check if the board is RPi5
check_rpi_board()
{
	model=$(tr -d '\0' </proc/device-tree/model)
    if [[ $model == *"Raspberry Pi 5"* ]]; then
        echo "This is a $model."
    elif [[ $model == *"Raspberry Pi Compute Module 5"* ]]; then
        echo "This is a $model."
    else
        echo "This is a $model."
        echo "This is not a Raspberry Pi 5. Will exit."        
        exit 0;
    fi
}

#check if the kernel version is greater than 6.6.31
check_kernel_version() 
{
    kernel_version=$(uname -r | awk -F '+' '{print $1}') 

    ref_version="6.6.31"

    IFS='.' read -r k_major k_minor k_patch <<<"$kernel_version"
    IFS='.' read -r r_major r_minor r_patch <<<"$ref_version"
    # echo "ref version: $k_major $k_minor $k_patch"
    # echo "read version: $r_major $r_minor $r_patch"
    if ((k_major > r_major)) || \
       ((k_major == r_major && k_minor > r_minor)) || \
       ((k_major == r_major && k_minor == r_minor && k_patch >= r_patch)); then
        echo "Kernel version is $kernel_version, do not support unpacked format."
        g_unpacked_supported=0
    else
        echo "Kernel version is $kernel_version,support unpacked format."
        g_unpacked_supported=1
    fi
}

check_i2c_bus() {

    kernel_version=$(uname -r | awk -F '+' '{print $1}') 

    ref_version="6.6.62"

    IFS='.' read -r k_major k_minor k_patch <<<"$kernel_version"
    IFS='.' read -r r_major r_minor r_patch <<<"$ref_version"
    # echo "ref version: $k_major $k_minor $k_patch"
    # echo "read version: $r_major $r_minor $r_patch"
    if ((k_major > r_major)) || \
       ((k_major == r_major && k_minor > r_minor)) || \
       ((k_major == r_major && k_minor == r_minor && k_patch >= r_patch)); then
        if ((g_cm5 == 1));then
            I2CBUS_CAM1=0
        else
            I2CBUS_CAM1=11
        fi
        I2CBUS_CAM0=10
        
    else
        if ((g_cm5 == 1));then
            I2CBUS_CAM1=0
        else
            I2CBUS_CAM1=4
        fi
        I2CBUS_CAM1=6
    fi
    echo "Kernel version is $kernel_version, use i2c-$I2CBUS_CAM0 for CAM0 and i2c-$I2CBUS_CAM1 for CAM1."
}

probe_camera_entity() 
{
    local entity_name="$1"
    local entity_id="$2"
	local media_dev

    echo "Find camera entry: $entity_name $entity_id"

    for media_dev in /dev/media*; do
        # echo "Checking $media_dev..."
        entity_info=$(media-ctl -d $media_dev -p 2>/dev/null | grep "$entity_name $entity_id-0054")
        if [ -n "$entity_info" ]; then
			g_media_device=$media_dev
			echo "Found $entity_name $entity_id entity in $g_media_device"
            echo ""
			return 1
        fi
    done

    echo "Not found camera entry: $entity_name $entity_id"

	return 0
}

setup_camera_entity() 
{
    local media_fmt="$3"
    local pixel_fmt="$4"
    local width="$5"
    local height="$6"

	# echo "$g_media_device $g_video_device"
    echo ""
    echo "Setup camera entity: $1 $2-0054 with format:$media_fmt, width:$width, height:$height pixel_fmt:$pixel_fmt"

    #reset media setting
	media-ctl -d $g_media_device -r
	
    #enable "rp1-cfe-csi2_ch0":0 [ENABLED]-->/dev/video0
	media-ctl -d $g_media_device -l ''\''csi2'\'':4 -> '\''rp1-cfe-csi2_ch0'\'':0 [1]'
    
    #set media's setting
    media-ctl -d "$g_media_device" --set-v4l2 "'$1 $2-0054':0[fmt:${media_fmt}/${width}x${height} field:none]"
    media-ctl -d "$g_media_device" -V "'csi2':0 [fmt:${media_fmt}/${width}x${height} field:none]"
    media-ctl -d "$g_media_device" -V "'csi2':4 [fmt:${media_fmt}/${width}x${height} field:none]"
    #set video node
	v4l2-ctl -d $g_video_device --set-fmt-video=width=$width,height=$height,pixelformat=$pixel_fmt,colorspace=rec709,ycbcr=rec709,xfer=rec709,quantization=full-range
}

echo ""
echo "================ Thermal Camera Module Setting ================"
check_rpi_board;
check_kernel_version;
check_i2c_bus;
echo ""

valid_drivers=("tcam-vdo" "tcam-raw")

if [ $# -eq 0 ]; then
    g_tcam_vdo_drv="tvdo";
    g_tcam_raw_drv="traw";
elif [ $# -eq 1 ]; then
    if [ "$1" == ${valid_drivers[0]} ]; then
        g_tcam_vdo_drv="tvdo";
    elif [ "$1" == ${valid_drivers[1]} ]; then
        g_tcam_raw_drv="traw";
    else
        echo "Please provide a correct camera module name!"
        exit 0;
    fi      
else
    echo "Invalid parameter!"
    exit 0
fi

export TCAMVDO_DEVICE=""
export TCAMVDO_SUBDEV=""
if [ "$g_tcam_vdo_drv" == "tvdo" ]; then
    probe_camera_entity $g_tcam_vdo_drv $I2CBUS_CAM1

    if [ $? -eq 1 ]; then
 		echo "tvdo probed: media device is $g_media_device"

 		g_video_device=$(media-ctl -e rp1-cfe-csi2_ch0 -d $g_media_device)
 		g_video_subdevice=$(media-ctl -e "$g_tcam_vdo_drv $I2CBUS_CAM1-0054" -d $g_media_device)

        echo "video device is $g_video_device"
        echo "video subdevice is $g_video_subdevice"
 		
        setup_camera_entity $g_tcam_vdo_drv $I2CBUS_CAM1 $TCAMVDO_MEDIA_FMT $TCAMVDO_PIXEL_FMT $TCAMVDO_WIDTH $TCAMVDO_HEIGHT
 		
        echo "Set tvdo finish, plese get frame from $g_video_device and use $g_video_subdevice for camera setting"

        export TCAMVDO_DEVICE="$g_video_device"
        export TCAMVDO_SUBDEV="$g_video_subdevice"
 	else 
 		echo "NOT FOUND $g_tcam_vdo_drv $I2CBUS_CAM1"
 	fi
fi
echo ""

export TCAMRAW_DEVICE=""
export TCAMRAW_SUBDEV=""
if [ "$g_tcam_raw_drv" == "traw" ]; then
    probe_camera_entity $g_tcam_raw_drv $I2CBUS_CAM0

    if [ $? -eq 1 ]; then
 		echo "traw probed: media device is $g_media_device"
 		
        g_video_device=$(media-ctl -e rp1-cfe-csi2_ch0 -d $g_media_device)
 		g_video_subdevice=$(media-ctl -e "$g_tcam_raw_drv $I2CBUS_CAM0-0054" -d $g_media_device)

        echo "video device is $g_video_device"
        echo "video subdevice is $g_video_subdevice"	
 		
        setup_camera_entity $g_tcam_raw_drv $I2CBUS_CAM0 $TCAMRAW_MEDIA_FMT $TCAMRAW_PIXEL_FMT $TCAMRAW_WIDTH $TCAMRAW_HEIGHT 
 		
        echo "Set traw finish, plese get frame from $g_video_device and use $g_video_subdevice for camera setting"

        export TCAMRAW_DEVICE="$g_video_device"
        export TCAMRAW_SUBDEV="$g_video_subdevice"
 	else 
 		echo "NOT FOUND $g_tcam_raw_drv $I2CBUS_CAM0"
 	fi
fi
echo ""


echo $TCAMVDO_DEVICE
echo $TCAMVDO_SUBDEV

echo $TCAMRAW_DEVICE
echo $TCAMRAW_SUBDEV

sudo sed "/^export TCAMVDO_DEVICE/d" -i ~/.bashrc
sudo sed "/^export TCAMVDO_SUBDEV/d" -i ~/.bashrc

sudo sed "/^export TCAMRAW_DEVICE/d" -i ~/.bashrc
sudo sed "/^export TCAMRAW_SUBDEV/d" -i ~/.bashrc

sudo echo "export TCAMVDO_DEVICE=$TCAMVDO_DEVICE" >> ~/.bashrc
sudo echo "export TCAMVDO_SUBDEV=$TCAMVDO_SUBDEV" >> ~/.bashrc
sudo echo "export TCAMRAW_DEVICE=$TCAMRAW_DEVICE" >> ~/.bashrc
sudo echo "export TCAMRAW_SUBDEV=$TCAMRAW_SUBDEV" >> ~/.bashrc

# echo "TCAMVDO_DEVICE=" >> ~/.bashrc
# echo "TCAMVDO_SUBDEV=" >> ~/.bashrc
# echo "TCAMRAW_DEVICE=" >> ~/.bashrc
# echo "TCAMRAW_SUBDEV=" >> ~/.bashrc

source ~/.bashrc


echo "================ Thermal Camera Module Setting Finish ================"
