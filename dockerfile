FROM ubuntu:22.04 AS unzip

RUN sed -i s@/archive.ubuntu.com/@/mirrors.tuna.tsinghua.edu.cn/@g /etc/apt/sources.list && apt update
RUN apt-get install -y xz-utils bzip2
COPY qemu-2k1000-static.20240526.tar.xz /
RUN cd /tmp && tar xavf /qemu-2k1000-static.20240526.tar.xz
COPY qemu-prebuilt-7.0.0.tar.gz /qemu.tar.gz
RUN mkdir /qemu && cd /qemu && tar xavf /qemu.tar.gz
COPY riscv64--musl--bleeding-edge-2020.08-1.tar.bz2 /
RUN cd /opt && tar jxvf /riscv64--musl--bleeding-edge-2020.08-1.tar.bz2
COPY toolchain-loongarch64-linux-gnu-gcc8-host-x86_64-2022-07-18.tar.xz /
RUN cd /opt/ && tar xavf /toolchain-loongarch64-linux-gnu-gcc8-host-x86_64-2022-07-18.tar.xz
# COPY gcc-13.2.0-loongarch64-linux-gnu-nw.tgz /
# RUN cd /opt && tar xavf /gcc-13.2.0-loongarch64-linux-gnu-nw.tgz
COPY gcc-13.2.0-loongarch64-linux-gnu.tgz /
RUN cd /opt/ && tar xavf /gcc-13.2.0-loongarch64-linux-gnu.tgz
COPY loongarch64-linux-musl-cross.tgz /
RUN cd /opt/ && tar xavf /loongarch64-linux-musl-cross.tgz
COPY  riscv64-linux-musl-cross.tgz /
RUN cd /opt/ && tar xavf /riscv64-linux-musl-cross.tgz
# FROM ubuntu:18.04 AS build_stage
# LABEL maintainer="w-mj <wmj@alphamj.cn>"

# RUN sed -i s@/archive.ubuntu.com/@/mirrors.tuna.tsinghua.edu.cn/@g /etc/apt/sources.list && apt update
# RUN apt install -y --no-install-recommends git ca-certificates && update-ca-certificates
# RUN git clone --recursive https://github.com/kendryte/kendryte-gnu-toolchain
# RUN apt install -y --no-install-recommends \
#     autoconf automake autotools-dev curl libmpc-dev libmpfr-dev libgmp-dev gawk build-essential bison flex texinfo gperf libtool patchutils bc zlib1g-dev libexpat-dev
# RUN cd kendryte-gnu-toolchain && ./configure --prefix=/opt/kendryte-toolchain --with-cmodel=medany --with-arch=rv64imafc --with-abi=lp64f && make -j8

FROM ubuntu:22.04 AS qemu
RUN sed -i s@/archive.ubuntu.com/@/mirrors.tuna.tsinghua.edu.cn/@g /etc/apt/sources.list
RUN sed -i s@/security.ubuntu.com/@/mirrors.tuna.tsinghua.edu.cn/@g /etc/apt/sources.list && apt-get update
RUN apt-get install -y --no-install-recommends xz-utils git python3 python3-pip  python3-venv build-essential ninja-build pkg-config  libglib2.0-dev  libpixman-1-dev libslirp-dev
RUN python3 -m pip config set global.index-url https://pypi.tuna.tsinghua.edu.cn/simple
RUN python3 -m pip install tomli
COPY qemu-9.2.1.tar.xz .
RUN tar xf qemu-9.2.1.tar.xz \
    && cd qemu-9.2.1 \
    && ./configure --prefix=/qemu-bin-9.2.1 \
        --target-list=loongarch64-softmmu,riscv64-softmmu,aarch64-softmmu,x86_64-softmmu \
        --enable-slirp \
    && make -j$(nproc) \
    && make install
RUN rm -rf qemu-9.2.1 qemu-9.2.1.tar.xz


FROM ubuntu:22.04
# FROM docker.educg.net/cg/code-server:4.10.0-ubuntu-1
# COPY --from=build_stage /opt/kendryte-toolchain/ /opt/kendryte-toolchain/
ENV PATH="$PATH:/opt/kendryte-toolchain/bin:/root/.cargo/bin"

USER root
RUN sed -i s@/archive.ubuntu.com/@/mirrors.tuna.tsinghua.edu.cn/@g /etc/apt/sources.list
RUN sed -i s@/security.ubuntu.com/@/mirrors.tuna.tsinghua.edu.cn/@g /etc/apt/sources.list
RUN apt update && apt-get install -y wget 

ARG DEBIAN_FRONTEND noninteractive
ENV TZ=Aisa/Shanghai
RUN echo deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-19 main >> /etc/apt/sources.list
RUN wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc

RUN apt-get update && apt install -y --no-install-recommends git ca-certificates && update-ca-certificates
RUN apt install -y --no-install-recommends python3 python3-pip make curl sshpass openssh-client libc-dev
RUN pip3 config set global.index-url https://pypi.tuna.tsinghua.edu.cn/simple

RUN apt-get install -y git build-essential gdb-multiarch gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu libpixman-1-0 git

RUN pip install pytz Cython jwt jinja2 requests

RUN apt-get install --no-install-recommends -y libguestfs-tools qemu-utils linux-image-generic libncurses5-dev
RUN apt-get install --no-install-recommends -y autotools-dev automake texinfo
RUN apt-get install --no-install-recommends -y tini musl musl-tools musl-dev cmake libclang-19-dev 
ENV LIBGUESTFS_BACKEND=direct
RUN rm -rf /var/lib/apt/lists/*



RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | bash -s -- -y
RUN echo 'source $HOME/.cargo/env' >> $HOME/.bashrc
RUN mkdir -p $HOME/.cargo/ && echo "[source.crates-io]\nregistry = \"https://github.com/rust-lang/crates.io-index\"\nreplace-with = 'aliyun'\n[source.aliyun]\nregistry = \"sparse+https://mirrors.aliyun.com/crates.io-index/\"" >> $HOME/.cargo/config
RUN export PATH="$PATH:/root/.cargo/bin"


RUN rustup install nightly-2025-01-18
RUN rustup install nightly-2024-02-03
RUN rustup install nightly-2024-05-01
RUN rustup install nightly-2024-08-01
RUN rustup install nightly-2025-02-01
RUN rustup install nightly-2025-05-20
RUN rustup default nightly-2025-01-18
RUN rustup component add llvm-tools-preview
RUN rustup target add riscv64imac-unknown-none-elf
RUN rustup target add riscv64gc-unknown-none-elf
RUN rustup target add loongarch64-unknown-linux-gnu
RUN rustup target add loongarch64-unknown-none

RUN rustup component add clippy --toolchain nightly-2025-02-01
RUN rustup component add rust-src --toolchain nightly-2025-02-01
RUN rustup component add rustfmt --toolchain nightly-2025-02-01 
RUN rustup target add loongarch64-unknown-none --toolchain nightly-2025-02-01
RUN rustup target add riscv64gc-unknown-none-elf --toolchain nightly-2025-02-01
RUN rustup target add riscv64imac-unknown-none-elf --toolchain nightly-2025-02-01
RUN rustup target add x86_64-unknown-linux-gnu --toolchain nightly-2025-02-01
RUN rustup target add x86_64-unknown-none --toolchain nightly-2025-02-01
RUN rustup target add riscv64imac-unknown-none-elf --toolchain nightly-2025-01-18
RUN rustup target add riscv64gc-unknown-none-elf --toolchain nightly-2025-01-18
RUN rustup target add loongarch64-unknown-linux-gnu --toolchain nightly-2025-01-18
RUN rustup component add llvm-tools-preview --toolchain nightly-2025-01-18
RUN rustup component add llvm-tools-preview --toolchain nightly-2025-02-01
RUN rustup component add llvm-tools-preview --toolchain nightly-2024-08-01
RUN rustup target add x86_64-unknown-none --toolchain nightly-2024-08-01
RUN rustup target add aarch64-unknown-none-softfloat --toolchain nightly-2024-08-01
RUN rustup target add loongarch64-unknown-none --toolchain nightly-2024-08-01
RUN rustup target add loongarch64-unknown-linux-gnu --toolchain nightly-2024-08-01
RUN rustup target add riscv64gc-unknown-none-elf --toolchain nightly-2024-08-01
RUN rustup target add riscv64imac-unknown-none-elf --toolchain nightly-2024-08-01


RUN rustup target add riscv64imac-unknown-none-elf --toolchain nightly-2024-02-03
RUN rustup target add riscv64gc-unknown-none-elf --toolchain nightly-2024-02-03
RUN rustup target add loongarch64-unknown-linux-gnu --toolchain nightly-2024-02-03
RUN rustup component add llvm-tools-preview --toolchain nightly-2024-02-03

RUN rustup target add riscv64imac-unknown-none-elf --toolchain nightly-2024-05-01
RUN rustup target add riscv64gc-unknown-none-elf --toolchain nightly-2024-05-01
RUN rustup target add loongarch64-unknown-linux-gnu --toolchain nightly-2024-05-01
RUN rustup component add llvm-tools-preview --toolchain nightly-2024-05-01

RUN rustup target add riscv64gc-unknown-none-elf --toolchain nightly-2025-05-20
RUN rustup target add riscv64imac-unknown-none-elf --toolchain nightly-2025-05-20
RUN rustup target add loongarch64-unknown-none --toolchain nightly-2025-05-20
RUN rustup target add loongarch64-unknown-none-softfloat --toolchain nightly-2025-05-20
RUN rustup component add rust-src --toolchain nightly-2025-05-20
RUN rustup component add llvm-tools-preview --toolchain nightly-2025-05-20

RUN cargo +nightly-2024-02-03 install cargo-binutils --locked
RUN cargo +nightly-2024-05-01 install cargo-binutils
RUN cargo +nightly-2025-01-18 install cargo-binutils
RUN cargo +nightly-2025-02-01 install cargo-binutils
RUN cargo +nightly-2024-08-01 install cargo-binutils

COPY oscomp-dependencies-2023 /oscomp-dependencies-2023
#
#ENV http_proxy="http://host.docker.internal:10809"
#ENV https_proxy="http://host.docker.internal:10809"
#

RUN cd /oscomp-dependencies-2023 && cargo build
#
ENV http_proxy=""
ENV https_proxy=""


# https://github.com/riscv/riscv-gnu-toolchain
# COPY riscv /opt/riscv
# https://github.com/kendryte/kendryte-gnu-toolchain/releases/tag/v8.2.0-20190213
COPY kendryte-toolchain /opt/kendryte-toolchain

# COPY --from=unzip /qemu /qemu/
# ENV PATH /qemu/bin/:$PATH
COPY --from=unzip /opt/toolchain-loongarch64-linux-gnu-gcc8-host-x86_64-2022-07-18 /opt/toolchain-loongarch64-linux-gnu-gcc8-host-x86_64-2022-07-18
ENV LD_LIBRARY_PATH=/opt/kendryte-toolchain/bin/:$LD_LIBRARY_PATH
ENV PATH=/opt/toolchain-loongarch64-linux-gnu-gcc8-host-x86_64-2022-07-18/bin/:$PATH
# COPY riscv64--musl--bleeding-edge-2020.08-1.tar.bz2 /opt/
# RUN cd /opt && tar jxvf /opt/riscv64--musl--bleeding-edge-2020.08-1.tar.bz2
COPY --from=unzip /opt/riscv64--musl--bleeding-edge-2020.08-1 /opt/riscv64--musl--bleeding-edge-2020.08-1
ENV PATH=$PATH:/opt/riscv64--musl--bleeding-edge-2020.08-1/bin

# COPY --from=unzip /tmp/qemu /tmp/qemu
# ENV PATH $PATH:/tmp/qemu/bin

# COPY --from=unzip /opt/gcc-13.2.0-loongarch64-linux-gnu-nw /opt/gcc-13.2.0-loongarch64-linux-gnu-nw
# ENV PATH $PATH:/opt/gcc-13.2.0-loongarch64-linux-gnu-nw/bin
COPY --from=unzip /opt/gcc-13.2.0-loongarch64-linux-gnu /opt/gcc-13.2.0-loongarch64-linux-gnu
ENV PATH=/opt/gcc-13.2.0-loongarch64-linux-gnu/bin/:$PATH

COPY --from=unzip /opt/loongarch64-linux-musl-cross /opt/loongarch64-linux-musl-cross
ENV PATH=/opt/loongarch64-linux-musl-cross/bin:$PATH

RUN apt-get update && apt-get install -y fusefat libvirglrenderer-dev libsdl2-dev libgtk-3-dev

COPY --from=unzip /opt/riscv64-linux-musl-cross /opt/riscv64-linux-musl-cross
ENV PATH=/opt/riscv64-linux-musl-cross/bin:$PATH

COPY --from=qemu /qemu-bin-9.2.1 /opt/qemu-bin-9.2.1
ENV PATH=/opt/qemu-bin-9.2.1/bin:$PATH


# RUN rm /qemu/qemu.tar.gz && rm /opt/riscv64--musl--bleeding-edge-2020.08-1.tar.bz2

ENTRYPOINT ["tini", "--"]

# ENV LD_LIBRARY_PATH /opt/kendryte-toolchain/bin/

# RUN mkdir -p /kernel/
# RUN groupadd -r cguser && useradd --no-log-init -m -r -g cguser cguser

# USER cguser
