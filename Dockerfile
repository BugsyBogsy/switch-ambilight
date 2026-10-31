# Reproduces the toolchain used for the working v0.5 sysmodule build.
FROM devkitpro/devkita64:20260219
RUN apt-get update && apt-get install -y --no-install-recommends git ca-certificates && rm -rf /var/lib/apt/lists/*
# Atmosphère 1.12.1 uses FsStorageForBatchRead; the older bundled libnx lacks it.
RUN git clone https://github.com/switchbrew/libnx.git /tmp/libnx && \
    git -C /tmp/libnx checkout b156b4774fb2df7b93402169509266da7547fc4c && \
    make -C /tmp/libnx/nx -j4 && make -C /tmp/libnx/nx install && \
    rm -rf /tmp/libnx
# Atmosphère is volume-mounted into /ams and may belong to the host UID.
RUN git config --global --add safe.directory /ams
RUN grep -q FsStorageForBatchRead /opt/devkitpro/libnx/include/switch/services/fs.h
