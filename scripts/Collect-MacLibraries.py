"""Collect original licenses and GNU library sources using the installed MacPorts recipes."""
import concurrent.futures
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile

if sys.platform != 'darwin':
    raise SystemExit('This helper runs only on the configured Mac build host')
root = pathlib.Path(__file__).resolve().parents[1]
sources = root / 'build/unix/library-sources'
downloads = root / 'build/unix/downloads/runtime'
licenses = root / 'third_party/licenses-unix'
for directory in (sources, downloads, licenses):
    directory.mkdir(parents=True, exist_ok=True)
ports = ('libiconv', 'gettext-runtime', 'libunistring', 'libidn2', 'libpsl', 'brotli', 'nghttp2')
gnu = set(ports[:4])

def collect(port):
    details = subprocess.check_output(['/opt/local/bin/port', 'distfiles', port], text=True)
    filename = re.search(r'^\[([^]]+)\]', details, re.M).group(1)
    digest = re.search(r'^ sha256: ([0-9a-f]{64})$', details, re.M).group(1)
    urls = re.findall(r'^  (https://\S+)$', details, re.M)
    url = next(value for value in urls if value.startswith('https://distfiles.macports.org/'))
    archive = downloads / filename
    if not archive.is_file() or hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        subprocess.run(['/opt/local/bin/curl', '--fail', '--location', '--silent', '--show-error',
                        '--max-time', '90', '--retry', '1', '--output', str(archive), url], check=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise RuntimeError('Source digest mismatch: ' + port)
    with tarfile.open(archive) as package:
        for member in package.getmembers():
            leaf = pathlib.PurePosixPath(member.name).name
            if not member.isfile() or member.size > 2 * 1024 * 1024:
                continue
            if not leaf.upper().startswith(('COPYING', 'LICENSE', 'COPYRIGHT', 'AUTHORS')):
                continue
            label = re.sub(r'[^A-Za-z0-9_.-]', '_', member.name)
            (licenses / (port + '-' + label + '.txt')).write_bytes(package.extractfile(member).read())
    recipe = pathlib.Path(subprocess.check_output(['/opt/local/bin/port', 'file', port], text=True).strip())
    if not recipe.is_relative_to('/opt/local/var/macports/sources'):
        raise RuntimeError('Unexpected recipe path')
    if port in gnu:
        shutil.copy2(archive, sources / filename)
        shutil.copytree(recipe.parent, sources / 'macports-recipes' / port, dirs_exist_ok=True)
    version = subprocess.check_output(['/opt/local/bin/port', 'installed', port], text=True).strip()
    return {'port': port, 'installed': version, 'source_url': url, 'sha256': digest, 'file': filename,
            'source_included': port in gnu}

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    records = list(pool.map(collect, ports))
(sources / 'sources.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
(sources / 'README.txt').write_text(
    '随附原始 GNU 库源码及对应已安装 MacPorts 构建文件，摘要见 sources.json。\n'
    '动态库在 Drop.app/Contents/Frameworks；可按随附 Portfile 和补丁重建并替换。\n'
    '安装构建所需 MacPorts 工具后，可使用 port build 及 port destroot 构建库。\n'
    '替换库后需要重新执行 codesign --force --deep --sign - Drop.app 生成本地签名。\n'
    'Drop 的可复现构建入口为项目 scripts/Bootstrap-Unix.sh 和 scripts/Build-Unix.sh。\n', encoding='utf-8')
print(json.dumps({'libraries': len(records), 'gnu_sources': len(gnu)}, ensure_ascii=False))
