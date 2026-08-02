# syntax=docker/dockerfile:1

ARG BASE_IMAGE=debian:bookworm-slim@sha256:7b140f374b289a7c2befc338f42ebe6441b7ea838a042bbd5acbfca6ec875818
FROM ${BASE_IMAGE}

ARG DEBIAN_SNAPSHOT=20260701T000000Z
ARG USERNAME=cuac
ARG USER_UID=1000
ARG USER_GID=1000

COPY containers/development/clang-format-requirements.txt /tmp/clang-format-requirements.txt

RUN rm -f /etc/apt/sources.list.d/debian.sources \
    && printf '%s\n' \
        "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bookworm main" \
        "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bookworm-updates main" \
        "deb [check-valid-until=no] http://snapshot.debian.org/archive/debian-security/${DEBIAN_SNAPSHOT} bookworm-security main" \
        > /etc/apt/sources.list \
    && apt-get -o Acquire::Check-Valid-Until=false update \
    && apt-get install -y --no-install-recommends \
        binutils \
        build-essential \
        ca-certificates \
        ccache \
        cmake \
        curl \
        file \
        git \
        libcurl4-openssl-dev \
        make \
        ninja-build \
        openssh-client \
        pkg-config \
        python3 \
        python3-pip \
        python3-venv \
        rsync \
        sudo \
        unzip \
        util-linux \
    && python3 -m venv /opt/cuac-tools \
    && /opt/cuac-tools/bin/python -m pip install \
        --disable-pip-version-check --no-deps --require-hashes \
        -r /tmp/clang-format-requirements.txt \
    && rm -f /tmp/clang-format-requirements.txt \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid "${USER_GID}" "${USERNAME}" \
    && useradd --uid "${USER_UID}" --gid "${USER_GID}" --create-home --shell /bin/bash "${USERNAME}" \
    && install -d --mode=1777 /var/lib/cuac-dev \
    && printf '%s ALL=(root) NOPASSWD:ALL\n' "${USERNAME}" > "/etc/sudoers.d/${USERNAME}" \
    && chmod 0440 "/etc/sudoers.d/${USERNAME}"

ENV CCACHE_COMPRESS=true \
    CCACHE_MAXSIZE=5G \
    CUAC_DEV_CONTAINER=1 \
    CUAC_DEV_ROOT=/var/lib/cuac-dev/container \
    CUAC_CCACHE_DIR=/var/lib/cuac-dev/ccache \
    GEN=ninja \
    PATH=/opt/cuac-tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

WORKDIR /workspaces/cuac
USER ${USERNAME}

CMD ["sleep", "infinity"]
