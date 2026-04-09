# Multi-stage build for rippled with smart vault + smart contract support
# Target: linux/amd64

# ============================================================
# Stage 1: Build
# ============================================================
FROM --platform=linux/amd64 ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    python3 \
    python3-pip \
    pipx \
    libgmp-dev \
    libgmpxx4ldbl \
    libssl-dev \
    libsodium-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Conan via pipx
RUN pipx install conan && pipx ensurepath
ENV PATH="/root/.local/bin:${PATH}"

# Setup Conan
RUN conan profile detect

# Add XRPL Conan remote
RUN conan remote add --index 0 xrplf https://conan.ripplex.io

# Copy source code
WORKDIR /rippled
COPY . .

# Install conan profiles
RUN conan config install conan/profiles/ -tf $(conan config home)/profiles/ || true

# Create build directory and install dependencies
RUN mkdir -p build && cd build && \
    conan install .. \
        --output-folder . \
        --build missing \
        --settings build_type=Release \
        --lockfile="" \
    2>&1 | tail -5

# Configure CMake
RUN cd build && \
    cmake \
        -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -Dxrpld=ON \
        -Dtests=OFF \
        .. \
    2>&1 | tail -5

# Build rippled
RUN cd build && cmake --build . --parallel $(nproc) 2>&1 | tail -10

# Verify the binary was created
RUN ls -la build/xrpld && build/xrpld --version

# ============================================================
# Stage 2: Runtime
# ============================================================
FROM --platform=linux/amd64 ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Install minimal runtime dependencies
RUN apt-get update && apt-get install -y \
    libgmp10 \
    libgmpxx4ldbl \
    libsodium23 \
    libssl3t64 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create rippled user
RUN useradd -m -s /bin/bash rippled

# Copy the binary
COPY --from=builder /rippled/build/xrpld /usr/local/bin/rippled

# Copy standalone config
COPY --from=builder /rippled/cfg/standalone.cfg /etc/rippled/rippled.cfg

# Create data directories
RUN mkdir -p /var/lib/rippled /var/log/rippled && \
    chown -R rippled:rippled /var/lib/rippled /var/log/rippled /etc/rippled

EXPOSE 6006 51235 5005

USER rippled
WORKDIR /var/lib/rippled

ENTRYPOINT ["rippled"]
CMD ["--conf", "/etc/rippled/rippled.cfg"]
