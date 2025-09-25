# fmruby-graphics-audio

## build

### set target

docker run --rm --group-add=dialout --group-add=plugdev --privileged $DEVICE_ARGS --user $(id -u):$(id -g) -v $PWD:/project -v /dev/bus/usb:/dev/bus/usb esp32_build_container:v5.5.1 idf.py set-target esp32

### build

docker run --rm --group-add=dialout --group-add=plugdev --privileged $DEVICE_ARGS --user $(id -u):$(id -g) -v $PWD:/project -v /dev/bus/usb:/dev/bus/usb esp32_build_container:v5.5.1 idf.py build
