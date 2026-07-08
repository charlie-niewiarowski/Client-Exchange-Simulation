FROM --platform=linux/amd64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    python3 \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
CMD ["bash"]
