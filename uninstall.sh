#!/bin/bash

if [ "$EUID" -ne 0 ]
	then echo "Please Run As Root!"
	exit
fi

echo "Uninstalling ..." && \
killall safed && \
sleep 5 && \
rmmod safe && \
echo "OK !"
