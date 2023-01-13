#!/usr/bin/env bash

set -eu

xhost +

sudo docker run \
    -it \
    --rm \
    --net=host \
    --runtime nvidia \
    -e DISPLAY=$DISPLAY \
    --device /dev/video0 \
    -v /tmp/.X11-unix/:/tmp/.X11-unix \
    -v $(pwd)/model:/deepstream-test1-usb-people-count/model \
    deepstream-test1-usb-people-count:1 ./deepstream-test1-usb-people-count camera
