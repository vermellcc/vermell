# Build stage: compile the library with a throwaway toolchain.
FROM debian:trixie-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt README.md ./
COPY cmake ./cmake
COPY sources ./sources
COPY include ./include

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel \
    && DESTDIR=/stage cmake --install build

# Runtime image: library, headers and the toolchain used to build against it.
# Same base and same apt line as the build stage, so the layer is shared.
FROM debian:trixie-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /stage/usr/local /usr/local

LABEL org.opencontainers.image.title="vermell" \
      org.opencontainers.image.description="Debian with the Vermell web framework (C++20) installed" \
      org.opencontainers.image.vendor="vermell" \
      org.opencontainers.image.version="1.0.0"
