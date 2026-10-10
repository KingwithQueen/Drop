"""Inspect real release ZIP contents and every manifest digest without running fixtures."""
import argparse
import hashlib
import json
import pathlib
import zipfile
import time
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(); parser.add_argument('archives',nargs='+',type=pathlib.Path); args=parser.parse_args(); records=[]
    for archive in args.archives:
        archive=archive.resolve(); assert archive.is_relative_to((ROOT/'build/packages').resolve())
        assert hashlib.sha256(archive.read_bytes()).hexdigest()==pathlib.Path(str(archive)+'.sha256').read_text().strip()
        with zipfile.ZipFile(archive) as z:
            names=z.namelist(); assert len(names)==len(set(name.casefold() for name in names))
            for name in names: assert not name.startswith('/') and ':' not in name and '..' not in pathlib.PurePosixPath(name).parts
            roots={name.split('/')[0] for name in names}; assert roots=={archive.stem}; prefix=archive.stem+'/'
            manifest=json.loads(z.read(prefix+'files.json').decode('utf-8-sig')); expected={item['path'].replace('\\','/') for item in manifest}
            actual={name[len(prefix):] for name in names if not name.endswith('/')}; assert actual==expected|{'files.json'}
            for item in manifest:
                content=z.read(prefix+item['path'].replace('\\','/')); assert len(content)==item['bytes'] and hashlib.sha256(content).hexdigest()==item['sha256']
            assert {'drop.exe','dropserver.exe','LICENSE.txt','README.md','docs/GOOGLE_DRIVE.md'}<=expected
            assert not any(pathlib.PurePosixPath(name).suffix.lower() in ('.db','.dpapi','.pem','.pdb','.lib','.obj') for name in actual)
            for exe in ('drop.exe','dropserver.exe'):
                content=z.read(prefix+exe)
                for fixture in ('test-only-password-123','backup-fixture-password-123','isolated-drive-refresh-fixture','DROP_TEST_'):
                    assert fixture.encode() not in content and fixture.encode('utf-16le') not in content
            records.append({'archive':str(archive),'files':len(actual),'bytes':archive.stat().st_size,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'result':'PASS'})
            print(f'PASS actual ZIP structure, full SHA-256 manifest, required documents/licenses and excluded secrets: {archive.name}',flush=True)
    output=ROOT/f'build/tests/packages-{time.time_ns()}.json'; output.write_text(json.dumps(records,indent=2),encoding='utf-8'); print(f'Package verification: {output}',flush=True)
if __name__=='__main__': main()
