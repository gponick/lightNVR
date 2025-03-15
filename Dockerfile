FROM ubuntu:24.04 AS builder

# Install dependencies
RUN apt update && apt install -y \
    build-essential \
    cmake \
    pkg-config \
    libsqlite3-dev \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libmicrohttpd-dev \
    libcurl4-openssl-dev \
    libssl-dev

# Set the working directory
WORKDIR /app

# Copy the source code
COPY . /app

# Build the application
RUN mkdir -p build/Release
WORKDIR /app/build/Release

RUN cmake -DCMAKE_BUILD_TYPE=Release ../..

RUN cmake --build . -- -j$(nproc)

FROM ubuntu:24.04 AS final

# Install runtime dependencies
RUN apt update \ 
    && apt install -y \
    libavcodec60 \
    libsqlite3-0 \
    libavformat60

# Set the working directory
WORKDIR /app

# Copy the built application from the builder stage
COPY --from=builder /app/build/Release/lightnvr /app/lightnvr
RUN mkdir -p /var/lib/lightnvr
COPY --from=builder /app/web /var/lib/lightnvr/www

CMD [ "/app/lightnvr", "-c", "/etc/lightnvr.conf" ]

