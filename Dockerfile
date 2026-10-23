FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    python3-venv \
    git \
    curl \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/pio
ENV PATH="/opt/pio/bin:$PATH"
RUN pip install --no-cache-dir "platformio==6.1.16"

WORKDIR /workspace
CMD ["pio", "run"]
