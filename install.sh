#!/bin/bash

if [ "$EUID" -ne 0 ]
	then echo "Please Run As Root!"
	exit
fi

echo "Installing ..." && \
apt update -qq && \
apt install -qq -y libsqlite3-dev libext2fs-dev libgtk-3-dev pkg-config && \
make -C kernel/ && \
gcc -DSQLITE_OMIT_LOAD_EXTENSION user/safed.c -lsqlite3 -lext2fs -o safed && \
gcc user/cli.c -o cli && \
gcc user/gui.c -o gui `pkg-config --cflags --libs gtk+-3.0` && \
insmod kernel/safe.ko && \
echo "OK !" && \
setsid ./safed
