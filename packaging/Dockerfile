ARG BASE_IMAGE
FROM ${BASE_IMAGE}
ARG UBUNTU=24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential pkg-config ninja-build python3 python3-venv python3-pip \
    dpkg-dev patchelf ca-certificates curl xz-utils gettext libxml2-dev \
    libjson-glib-dev libsecret-1-dev libpcre2-dev libffi-dev libmount-dev \
    libselinux1-dev zlib1g-dev libelf-dev libepoxy-dev libgraphene-1.0-dev \
    libpango1.0-dev libgdk-pixbuf-2.0-dev libfribidi-dev libfontconfig1-dev \
    libx11-dev libxext-dev libxi-dev libxrandr-dev libxcursor-dev libxdamage-dev \
    libxfixes-dev libxcomposite-dev libxinerama-dev libxkbcommon-dev \
    libwayland-dev wayland-protocols liblz4-dev libgnutls28-dev libicu-dev \
    libsystemd-dev libexpat1-dev \
    xvfb xauth xdotool dbus-x11 gnome-keyring openssh-client inetutils-telnet \
    picocom adwaita-icon-theme librsvg2-common shared-mime-info desktop-file-utils \
    && if [ "$UBUNTU" = 24.04 ]; then \
       apt-get install -y --no-install-recommends libgtk-4-dev libvte-2.91-gtk4-dev; fi \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 1001 devterm-test
RUN python3 -m venv /opt/build-tools \
    && /opt/build-tools/bin/pip install --no-cache-dir meson==1.3.2 packaging==24.2 asyncssh==2.24.1
ENV PATH=/opt/build-tools/bin:$PATH
RUN apt-get update && apt-get install -y --no-install-recommends libdrm-dev sassc gperf bison flex \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
