#!/bin/bash
apt-get update
cd /root/checkmk

#./buildscripts/infrastructure/build-nodes/gnu-toolchain/bw-build-gnu-toolchain.sh

#mv /usr/bin/gcc /usr/bin/gcc-old \
#    && mv /usr/bin/cc /usr/bin/cc-old \
#    && ln -s /usr/bin/gcc-8 /usr/bin/gcc \
#    && ln -s /usr/bin/gcc-8 /usr/bin/cc

make -C omd setup
DEBFULLNAME="Vaclav Chlumsky" DEBEMAIL="vchlumsky@cesnet.cz" make deb
