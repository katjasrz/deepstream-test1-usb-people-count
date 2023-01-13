ARG BASE_IMAGE=nvcr.io/nvidia/deepstream-l4t:6.1.1-triton
FROM ${BASE_IMAGE}

ARG REPOSITORY_NAME=deepstream-test1-usb-people-count

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG C.UTF-8
ENV PATH="/usr/local/cuda/bin:${PATH}"
ENV LD_LIBRARY_PATH="/usr/local/cuda/lib64:${LD_LIBRARY_PATH}"

WORKDIR /tmp

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        libgstreamer1.0-dev \
        libgstreamer-plugins-base1.0-dev \
        libgstrtspserver-1.0-dev \
        libx11-dev \
        libjson-glib-dev \
        graphviz && \
    rm -rf /var/lib/apt/lists/*
    
RUN mkdir /${REPOSITORY_NAME}
COPY ./ /${REPOSITORY_NAME}

WORKDIR /${REPOSITORY_NAME}

RUN make

#CMD ./deepstream-test1-usb-people-count    
