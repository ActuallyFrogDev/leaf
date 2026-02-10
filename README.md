# leaf

![Leaf logo](/web/static/images/logo.png)

Leaf is a package manager for Linux that is meant to be **simple, user-friendly, and easy-to-use.**

## Installation

**NOTE: The only supported distros now are Arch, Debian, Ubuntu, Fedora, Opensuse, and Nix. If you have a different distro, you will have to do a Manual Installation.**

Installation is as easy as just running this command in your Linux terminal:
```bash
/bin/bash -c "$(curl -fsSL https://leaf.treelinux.org/install/install.sh)"
```

And **boom**! No pain, no troubleshooting; Leaf installs **automatically**, you don't have to do anything!

## Manual Installation

This is for when you dont want to run the install script, or you have an unsupported distribution.

```bash
git clone https://github.com/ActuallyFrogDev/leaf
cd leaf
make leaf
```

## Instructions
### Install package
```bash
leaf grow <pkg>
```

### Remove package
```bash
leaf uproot <pkg>
```
### List installed packages
```bash
leaf list
```

### Reset leaf entirely
```bash
leaf reset
```
