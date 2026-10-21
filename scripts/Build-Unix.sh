#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PATH="/opt/local/bin:/opt/local/sbin:$PATH"
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
export PKG_CONFIG_PATH="$root/build/unix/deps/lib/pkgconfig:$root/build/unix/deps/lib64/pkgconfig:/opt/local/libexec/openssl3/lib/pkgconfig:/opt/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
config=${1:-Debug}
case "$config" in Debug|Release) ;; *) echo 'Usage: Build-Unix.sh [Debug|Release]' >&2; exit 2;; esac
testing=ON
# 检查代码仅在开发机保留；源码发布缺少这些文件时覆盖旧缓存，关闭测试目标。
gui_check=gtk_probe.inc
if [ "$(uname -s)" = Darwin ]; then gui_check=macos_probe_c.inc; fi
for check_source in portable_check.c client_fixture.c "$gui_check"; do
    if [ ! -f "$root/checks/$check_source" ]; then testing=OFF; fi
done
cmake -S "$root" -B "$root/build/unix/$config" -G Ninja -DCMAKE_BUILD_TYPE="$config" -DBUILD_TESTING="$testing"
cmake --build "$root/build/unix/$config" --parallel 2
if [ "$testing" = ON ]; then
    ctest --test-dir "$root/build/unix/$config" --output-on-failure
else
    echo 'Local verification sources are not present; built the product without tests.'
fi
"$root/build/unix/$config/drop" --version
