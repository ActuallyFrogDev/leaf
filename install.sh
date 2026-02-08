#!/bin/bash

DISTRO=$( cat /etc/*-release | tr [:upper:] [:lower:] | grep -Poi '(arch|debian|ubuntu|nix)' | uniq )
if [ -z "$DISTRO" ]; then
    DISTRO='unknown'
fi

case "$DISTRO" in
    arch|debian|ubuntu|nix)
        echo "Linux Distribution: $DISTRO"
        echo "Starting Installation..."
        ;;
    unknown)
        echo "Your Linux Distribution is not supported by leaf. Look at the README.md on our github page to start a manual installation."
        ;;
esac

