# Ubuntu 20.04 (focal-20250404), official image index digest.
FROM ubuntu@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214

ARG DEBIAN_FRONTEND=noninteractive
ARG REPO_TAG=v2.54
ARG REPO_COMMIT=97dc5c1bd9527c2abe2183b16a4b7ef037dc34a7
ARG BUILD_UID=1000
ARG BUILD_GID=1000

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        abootimg \
        adb \
        android-sdk-libsparse-utils \
        bc \
        binwalk \
        bison \
        build-essential \
        ca-certificates \
        ccache \
        cpio \
        curl \
        device-tree-compiler \
        fastboot \
        file \
        flex \
        g++-multilib \
        gcc-multilib \
        git \
        git-lfs \
        gnupg \
        gperf \
        heimdall-flash \
        imagemagick \
        jq \
        libc6-dev-i386 \
        lib32ncurses5-dev \
        lib32readline-dev \
        lib32z1-dev \
        libdw-dev \
        libelf-dev \
        libncurses5 \
        libncurses5-dev \
        libsdl1.2-dev \
        libssl-dev \
        libxml2 \
        libxml2-utils \
        locales \
        lz4 \
        lzop \
        mkbootimg \
        pngcrush \
        protobuf-compiler \
        python-is-python2 \
        python2 \
        python3 \
        python3-protobuf \
        rsync \
        schedtool \
        shellcheck \
        squashfs-tools \
        strace \
        unzip \
        wget \
        xsltproc \
        xz-utils \
        zip \
        zlib1g-dev \
    && git clone --depth=1 --branch "${REPO_TAG}" \
        https://gerrit.googlesource.com/git-repo /tmp/git-repo \
    && test "$(git -C /tmp/git-repo rev-parse HEAD)" = "${REPO_COMMIT}" \
    && install -m 0755 /tmp/git-repo/repo /usr/local/bin/repo \
    && rm -rf /tmp/git-repo \
    && locale-gen en_US.UTF-8 \
    && git lfs install --system \
    && groupadd --gid "${BUILD_GID}" forge \
    && useradd --uid "${BUILD_UID}" --gid "${BUILD_GID}" --home-dir /tmp/home --shell /bin/bash forge \
    && rm -rf /var/lib/apt/lists/*

ENV LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8 \
    USE_CCACHE=1 \
    CCACHE_EXEC=/usr/bin/ccache \
    CCACHE_DIR=/ccache

WORKDIR /android
CMD ["bash"]
