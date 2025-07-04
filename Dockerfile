FROM debian:12

RUN dpkg --add-architecture i386 && \
	apt-get update && \
	apt-get install -y \
	build-essential \
	gcc-multilib \
	g++-multilib \
	cmake \
	curl \
	git \
	pkg-config \
	libcurl4-gnutls-dev:i386 \
	libssl-dev:i386 \
	zlib1g-dev:i386 \
	libzstd-dev:i386 \
	libstdc++-12-dev:i386 \
	file
	
ENV PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig

WORKDIR /opt/project
COPY . .

RUN mkdir build && cd build && \
	cmake -DCMAKE_C_FLAGS="-m32" \
	-DCMAKE_CXX_FLAGS="-m32" \
	-DCMAKE_EXE_LINKER_FLAGS="-m32" .. && \
	make -j$(nproc)