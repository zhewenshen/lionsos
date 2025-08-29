#!/bin/bash

export MICROKIT_SDK=$HOME/Documents/lionsos-fork/microkit-sdk-2.0.1
export MICROKIT_BOARD=qemu_virt_aarch64
export NFS_SERVER=10.0.2.2
export NFS_DIRECTORY=/test
export WEBSITE_DIR=www
export MICROKIT_CONFIG=debug

if ! dpkg -l | grep -q nfs-kernel-server; then
    echo "Installing NFS server..."
    sudo apt-get update
    sudo apt-get install -y nfs-kernel-server
else
    echo "NFS server already installed"
fi

sudo mkdir -p $NFS_DIRECTORY
sudo mkdir -p $NFS_DIRECTORY/$WEBSITE_DIR

echo "Hello World!" | sudo tee $NFS_DIRECTORY/$WEBSITE_DIR/index.html

echo "$NFS_DIRECTORY *(rw,sync,no_subtree_check,no_root_squash,insecure)" | sudo tee /etc/exports

sudo systemctl enable nfs-kernel-server
sudo systemctl restart nfs-kernel-server
sudo exportfs -ra

if ! sudo iptables -L INPUT -n | grep -q "dpt:2049"; then
    sudo iptables -A INPUT -p tcp --dport 2049 -j ACCEPT
    sudo iptables -A INPUT -p udp --dport 2049 -j ACCEPT
fi

echo "Building C webserver..."
make -j8

echo "Starting QEMU with C webserver..."
make qemu
