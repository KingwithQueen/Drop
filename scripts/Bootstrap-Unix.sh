#!/bin/sh
# 固定源码及 SHA-512 来自现有 vcpkg baseline；仅写入本项目 build/unix，不安装系统软件。
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
base="$root/build/unix"
prefix="$base/deps"
export PATH="/opt/local/bin:/opt/local/sbin:$PATH"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig:/opt/local/libexec/openssl3/lib/pkgconfig:/opt/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
mkdir -p "$base/downloads" "$base/sources" "$prefix"
fetch() {
    name=$1
    url=$2
    expected=$3
    archive="$base/downloads/$name.tar.gz"
    if [ ! -f "$archive" ]; then curl --fail --location --retry 2 --connect-timeout 15 "$url" -o "$archive.part"; mv "$archive.part" "$archive"; fi
    actual=$(openssl dgst -sha512 "$archive" | awk '{print $NF}')
    if [ "$actual" != "$expected" ]; then echo "SHA-512 mismatch: $name" >&2; exit 1; fi
    if [ ! -d "$base/sources/$name" ]; then mkdir "$base/sources/$name"; tar -xzf "$archive" --strip-components=1 -C "$base/sources/$name"; fi
}
if [ ! -f "$prefix/lib/libcjson.a" ] && [ ! -f "$prefix/lib64/libcjson.a" ]; then
    fetch cjson-1.7.19 https://codeload.github.com/DaveGamble/cJSON/tar.gz/v1.7.19 7b22bdd05b8e0bf8b24ab79db128dbab9f8c2c167f4b1d3073922b8ff1e0f0e8917322283d6d4091002f014f42692984f4f641761f05965d3b66fb3eca14dc46
    cmake -S "$base/sources/cjson-1.7.19" -B "$base/cjson" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF
    cmake --build "$base/cjson" --parallel 2
    cmake --install "$base/cjson"
fi
if [ ! -f "$prefix/lib/libsodium.a" ] && [ ! -f "$prefix/lib64/libsodium.a" ]; then
    fetch sodium-1.0.22 https://codeload.github.com/jedisct1/libsodium/tar.gz/1.0.22-RELEASE f8e11ad193037b7b885a40b832da331105f4e5943b74c0297fe07e02a313786016c777aebb094ad1fcf16398e54af545c2d55404c28b371eae5922d1e164ba00
    (cd "$base/sources/sodium-1.0.22"; ./autogen.sh; ./configure --prefix="$prefix" --disable-shared --enable-static; make -j2; make install)
fi
if [ ! -f "$prefix/lib/libsqlite3.a" ]; then
    fetch sqlite-3.53.4 https://sqlite.org/2026/sqlite-autoconf-3530400.tar.gz c24374e9393a943157f533f96e89e6c5743e5f5aad169d8393cff3088ca5ccbe5cc0561681ace49c349d0fe402298ee2624d319f422967247ce0792e3b3aa01e
    mkdir -p "$prefix/lib/pkgconfig" "$prefix/include"
    cc -std=c17 -O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c "$base/sources/sqlite-3.53.4/sqlite3.c" -o "$base/sqlite3.o"
    ar rcs "$prefix/lib/libsqlite3.a" "$base/sqlite3.o"
    cp "$base/sources/sqlite-3.53.4/sqlite3.h" "$prefix/include/"
    cat > "$prefix/lib/pkgconfig/sqlite3.pc" <<EOF
prefix=$prefix
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: SQLite
Description: Drop pinned SQLite
Version: 3.53.4
Libs: -L\${libdir} -lsqlite3 -lpthread -lm
Cflags: -I\${includedir}
EOF
fi
pkg-config --modversion libcjson libsodium sqlite3 libzstd openssl libcurl
