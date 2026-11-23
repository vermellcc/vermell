# Build stage: the toolchain only lives here, it is not shipped.
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

# Runtime image: library and headers only.
FROM debian:trixie-slim

COPY --from=builder /stage/usr/local /usr/local

LABEL org.opencontainers.image.title="vermell" \
      org.opencontainers.image.description="Debian with the Vermell web framework (C++20) installed" \
      org.opencontainers.image.vendor="vermell" \
      org.opencontainers.image.version="1.0.0"
