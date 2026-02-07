#!/bin/bash

DISTRO=$( cat /etc/*-release | tr [:upper:] [:lower:] | grep -Poi '(arch|debian|ubuntu|nix)' | uniq )
if [ -z $DISTRO ]; then
    DISTRO='unknown'
fi

if [ $DISTRO == "arch" ]; then
    echo "Linux Distribution: $DISTRO"
    echo "Starting Installation..."
fi

if [ $DISTRO == "debian" ]; then
    echo "Linux Distribution: $DISTRO"
    echo "Starting Installation..."
fi

if [ $DISTRO == "ubuntu" ]; then
    echo "Linux Distribution: $DISTRO"
    echo "Starting Installation..."
fi

if [ $DISTRO == "nix" ]; then
    echo "Linux Distribution: $DISTRO"
    echo "Starting Installation..."
fi

if [ $DISTRO == "unknown" ]; then
    echo "Your Linux Distribution is not supported by leaf. Look at the README.md on our github page to start a manual installation."
fi

