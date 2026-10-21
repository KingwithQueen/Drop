"""Package native clients in the project build directory; never include credentials or test drivers."""
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import time

root=pathlib.Path(__file__).resolve().parents[1]
configuration=sys.argv[1] if len(sys.argv)>1 else 'Release'
if configuration!='Release': raise SystemExit('Distribution requires Release')
version=re.search(r'^#define DROP_VERSION "([0-9]+\.[0-9]+\.[0-9]+)"', (root/'dropcommon/version.h').read_text(),re.M).group(1)
system='macos' if sys.platform=='darwin' else 'linux'
arch=platform.machine()
name=f'Drop-{version}-{system}-{arch}'
build=root/'build/unix/Release'
stage=root/'build/unix/packages'/(name+'-'+str(time.time_ns()))
package=stage/name
package.mkdir(parents=True)
binary=build/'drop'
if not binary.is_file(): raise SystemExit('Build Release first')

def run(*args): return subprocess.check_output(list(map(str,args)),text=True).strip()

if system=='macos':
    import plistlib
    subprocess.run([sys.executable,str(root/'scripts/Collect-MacLibraries.py')],check=True)
    app=package/'Drop.app'
    contents=app/'Contents'
    executable=contents/'MacOS/drop'
    frameworks=contents/'Frameworks'
    resources=contents/'Resources'
    executable.parent.mkdir(parents=True); frameworks.mkdir(); resources.mkdir()
    shutil.copy2(binary,executable)
    metadata={'CFBundleExecutable':'drop','CFBundleIdentifier':'win.jsnowly.Drop','CFBundleName':'Drop','CFBundleDisplayName':'Drop',
        'CFBundlePackageType':'APPL','CFBundleShortVersionString':version,'CFBundleVersion':version,'CFBundleIconFile':'Drop.icns',
        'LSMinimumSystemVersion':'13.7','NSHighResolutionCapable':True,'NSPrincipalClass':'NSApplication'}
    (contents/'Info.plist').write_bytes(plistlib.dumps(metadata))
    shutil.copy2(root/'Dropico.png',resources/'Drop.png')
    iconset=stage/'Drop.iconset'; iconset.mkdir()
    for size in (16,32,128,256,512):
        for scale in (1,2):
            target=iconset/f'icon_{size}x{size}{"@2x" if scale==2 else ""}.png'
            subprocess.run(['sips','-z',str(size*scale),str(size*scale),str(root/'Dropico.png'),'--out',str(target)],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['iconutil','-c','icns',str(iconset),'-o',str(resources/'Drop.icns')],check=True)
    ca=pathlib.Path(run('/opt/local/bin/curl-config','--ca'))
    shutil.copy2(ca,resources/'ca-bundle.pem')
    copied={}
    def dependencies(path):
        lines=run('otool','-L',path).splitlines()[1:]
        return [line.strip().split(' (')[0] for line in lines]
    queue=[executable]
    while queue:
        item=queue.pop()
        for dependency in dependencies(item):
            if dependency.startswith(('/System/','/usr/lib/','@')): continue
            original=pathlib.Path(dependency).resolve()
            if not original.is_relative_to('/opt/local'): raise RuntimeError('Unexpected dependency boundary: '+dependency)
            destination=frameworks/pathlib.Path(dependency).name
            if destination.name in copied and copied[destination.name]!=original: raise RuntimeError('Library name collision')
            if destination.name not in copied:
                shutil.copy2(original,destination); destination.chmod(0o755); copied[destination.name]=original; queue.append(destination)
            replacement=('@executable_path/../Frameworks/' if item==executable else '@loader_path/')+destination.name
            subprocess.run(['install_name_tool','-change',dependency,replacement,str(item)],check=True)
        if item!=executable: subprocess.run(['install_name_tool','-id','@loader_path/'+item.name,str(item)],check=True)
    for library in frameworks.iterdir(): subprocess.run(['codesign','--force','--sign','-',str(library)],check=True,capture_output=True)
    run(executable,'--version')
    for item in [executable,*frameworks.iterdir()]:
        if any(dep.startswith('/opt/local') for dep in dependencies(item)): raise RuntimeError('Unbundled MacPorts dependency')
    dependency_licenses=resources/'licenses'; dependency_licenses.mkdir()
    docs=pathlib.Path('/opt/local/share/doc')
    curl_license=docs/'curl/COPYING'
    if curl_license.is_file(): shutil.copy2(curl_license,dependency_licenses/'curl-COPYING.txt')
    subprocess.run(['codesign','--force','--deep','--sign','-',str(app)],check=True,capture_output=True)
    subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True,capture_output=True)
    library_sources=root/'build/unix/library-sources'
    if not library_sources.is_dir(): raise RuntimeError('Prepare bundled library sources and licenses first')
    shutil.copytree(library_sources,package/'library-sources')
else:
    shutil.copy2(binary,package/'drop')
    icons=package/'share/icons/hicolor/256x256/apps'; icons.mkdir(parents=True)
    shutil.copy2(root/'Dropico.png',icons/'win.jsnowly.Drop.png')
    applications=package/'share/applications'; applications.mkdir(parents=True)
    (applications/'win.jsnowly.Drop.desktop').write_text('[Desktop Entry]\nType=Application\nName=Drop\nExec=drop\nIcon=win.jsnowly.Drop\nCategories=Network;Utility;\nTerminal=false\n',encoding='utf-8')
    run(package/'drop','--version')
for filename in ('README.md','LICENSE.txt'):
    if (root/filename).is_file(): shutil.copy2(root/filename,package/filename)
docs=package/'docs'; docs.mkdir()
for filename in ('CROSS_PLATFORM.md','PROTOCOL.md','PAKE.md'):
    if (root/'docs'/filename).is_file(): shutil.copy2(root/'docs'/filename,docs/filename)
licenses=package/'licenses'; licenses.mkdir()
csrp_licenses=licenses/'csrp'; csrp_licenses.mkdir()
# 许可证使用明确清单，不能递归带入仅本机保留的上游自测源码。
for filename in ('LICENSE','PROVENANCE.md'):
    shutil.copy2(root/'vendor/csrp'/filename,csrp_licenses/filename)
for source,license_name in [('cjson-1.7.19/LICENSE','cJSON-LICENSE.txt'),('sodium-1.0.22/LICENSE','libsodium-LICENSE.txt')]:
    path=root/'build/unix/sources'/source
    if path.is_file(): shutil.copy2(path,licenses/license_name)
copyrights=root/'third_party/licenses-unix'
if copyrights.is_dir(): shutil.copytree(copyrights,licenses/'runtime')
packaged_binary=package/'Drop.app/Contents/MacOS/drop' if system=='macos' else package/'drop'
(package/'BUILD-INFO.json').write_text(json.dumps({'product_version':version,'platform':system+'-'+arch,
    'build_label':os.environ.get('DROP_BUILD_LABEL',''),'binary_sha256':hashlib.sha256(packaged_binary.read_bytes()).hexdigest()},indent=2)+'\n',encoding='utf-8')
manifest=[]
for file in sorted(package.rglob('*')):
    if file.is_file(): manifest.append({'path':str(file.relative_to(package)),'sha256':hashlib.sha256(file.read_bytes()).hexdigest(),'bytes':file.stat().st_size})
(package/'manifest.json').write_text(json.dumps({'version':version,'system':system,'architecture':arch,'files':manifest},ensure_ascii=False,indent=2),encoding='utf-8')
archive=root/'build/unix/packages'/(name+'.tar.gz')
with tarfile.open(archive,'w:gz') as output: output.add(package,arcname=name)
print(json.dumps({'archive':str(archive),'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'package':str(package)},ensure_ascii=False))
