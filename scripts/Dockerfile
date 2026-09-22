# Build environment for force-cratedigger — Akai Force is ARM 32-bit
# hard-float (ELF 32-bit LSB, ARM EABI5, /lib/ld-linux-armhf.so.3,
# "GNU/Linux 3.2.0"). Same toolchain choice as force-maze/force-acid: build
# *natively for armhf under QEMU emulation* rather than cross-compile, so
# the glibc/GCC ABI is exactly the device's, not something we have to
# reason about separately. See force-maze/maze-voice/scripts/Dockerfile
# for the fuller writeup of why stretch (not buster) and why native-under-
# QEMU (not cross-compile) for a libpthread-only binary like this one.
FROM arm32v7/debian:stretch

RUN sed -i 's|deb.debian.org|archive.debian.org|g; /debian-security/d; /-updates/d' /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends \
        g++ \
        file \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
