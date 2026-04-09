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
    binutils \
    cmake \
    file \
    git \
    ninja-build \
    pkg-config \
    python3 \
    python3-pip \
    pipx \
    libgmp-dev \
    libgmpxx4ldbl \
    libssl-dev \
    libsodium-dev \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Install Rust (required by wasmi)
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
ENV PATH="/root/.cargo/bin:${PATH}"
RUN cargo install cbindgen

# Install Conan via pipx
RUN pipx install conan && pipx ensurepath
ENV PATH="/root/.local/bin:${PATH}"

# Write a static conan profile (skip auto-detection which fails in Docker)
RUN mkdir -p /root/.conan2/profiles && printf '\
[settings]\n\
os=Linux\n\
arch=x86_64\n\
build_type=Release\n\
compiler=gcc\n\
compiler.version=13\n\
compiler.cppstd=20\n\
compiler.libcxx=libstdc++11\n\
' > /root/.conan2/profiles/default

# Add XRPL Conan remote (patched recipes for wasmi, grpc, etc.)
RUN conan remote add --index 0 xrplf https://conan.ripplex.io

# Copy source code
WORKDIR /rippled
COPY . .

# Download and patch wasmi source before building
# The wasmi ExternalProject has an empty INSTALL_COMMAND that triggers
# a default "make install" which fails with Error 127 in Docker.
# We first run conan to download sources, then patch, then retry.
SHELL ["/bin/bash", "-o", "pipefail", "-c"]
RUN mkdir -p build && cd build && \
    conan install .. \
        --output-folder . \
        --build missing \
        --settings build_type=Release \
        --lockfile="" \
    || true && \
    find /root/.conan2 -path "*/wasmi*/c_api/CMakeLists.txt" \
        -exec sed -i 's|INSTALL_COMMAND "${WASMI_INSTALL_COMMAND}"|INSTALL_COMMAND ""|g' {} \; && \
    conan install .. \
        --output-folder . \
        --build missing \
        --settings build_type=Release \
        --lockfile=""

# Configure CMake
RUN cd build && \
    cmake \
        -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -Dxrpld=ON \
        -Dtests=OFF \
        ..

# Build rippled
RUN cd build && cmake --build . --parallel $(nproc)

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

# Create data and config directories
RUN mkdir -p /var/lib/rippled /var/log/rippled /etc/rippled && \
    chown -R rippled:rippled /var/lib/rippled /var/log/rippled /etc/rippled

# Ports: ws(6006) peer(51235) rpc(5005)
EXPOSE 6006 51235 5005

# Config is mounted at runtime — no default baked in
VOLUME ["/etc/rippled", "/var/lib/rippled"]

USER rippled
WORKDIR /var/lib/rippled

ENTRYPOINT ["rippled"]
CMD ["--conf", "/etc/rippled/rippled.cfg"]
