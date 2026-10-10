"""Online encrypted backup and older-snapshot recovery with actual native clients."""
import argparse
import hashlib
import http.client
import json
import os
import pathlib
import socket
import sqlite3
import ssl
import subprocess
import time
import uuid

ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--configuration',default='Debug')
    parser.add_argument('--large',action='store_true')
    parser.add_argument('--resume-case',type=pathlib.Path)
    args=parser.parse_args()
    binary=ROOT/f'build/bin/x64/{args.configuration}'
    server,client=binary/'dropserver.exe',binary/'drop.exe'
    root=args.resume_case.resolve() if args.resume_case else ROOT/f'build/tests/backup-{args.configuration}-中文-{uuid.uuid4().hex}'
    if args.resume_case:
        assert root.is_relative_to((ROOT/'build/tests').resolve()) and root.is_dir()
    else: root.mkdir(parents=True)
    data=root/'encrypted-server'
    backend=root/'encrypted-backups'
    report={'configuration':args.configuration,'checks':[],'result':'FAIL'}
    process=None
    def run(exe,*arguments,success=True):
        result=subprocess.run([str(exe),*map(str,arguments)],capture_output=True,timeout=3600 if args.large else 120)
        if success and result.returncode:
            raise RuntimeError(f'{arguments[0]} failed (exit {result.returncode})')
        return result
    def record(text):
        report['checks'].append({'name':text,'result':'PASS'})
        print(f'PASS {text}',flush=True)
    def wait_for(test,seconds=30):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if test(): return
            time.sleep(.2)
        raise RuntimeError('Timed out waiting for durable backup state')
    log=(root/'server.log').open('wb')
    try:
        if not args.resume_case: run(server,'--self-test',root)
        package=root/'recovery.bin'
        if not args.resume_case: run(server,'--test-recovery-export','--data',data,'--target',package)
        openssl=ROOT/'third_party/installed-x64/x64-windows/tools/openssl/openssl.exe'
        key,cert,pem=data/'key.pem',data/'cert.pem',data/'tls.pem'
        env=os.environ.copy(); env['OPENSSL_CONF']=str(openssl.parent/'openssl.cnf')
        subprocess.run([str(openssl),'req','-x509','-newkey','rsa:2048','-sha256','-nodes','-days','2','-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],capture_output=True,check=True,env=env,timeout=60)
        pem.write_bytes(cert.read_bytes()+key.read_bytes())
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0)); port=listener.getsockname()[1]
        url=f'https://localhost:{port}'
        context=ssl.create_default_context(cafile=str(cert))
        def request(path,body=None,token=None):
            c=http.client.HTTPSConnection('localhost',port,context=context,timeout=10)
            headers={'Content-Type':'application/json'}
            if token: headers['Authorization']=f'Bearer {token}'
            c.request('POST' if body is not None else 'GET',path,json.dumps(body,ensure_ascii=False).encode() if body is not None else None,headers)
            r=c.getresponse(); status,response=r.status,json.loads(r.read()); c.close(); return status,response
        def start(directory):
            child=subprocess.Popen([str(server),'serve','--data',str(directory),'--certificate',str(pem),'--listen',f'127.0.0.1:{port}','--backup-directory',str(backend)],stdout=log,stderr=log)
            def ready():
                if child.poll() is not None: raise RuntimeError('Server exited before ready')
                try: return request('/api/v1/server-info')[0]==200
                except (OSError,http.client.HTTPException): return False
            wait_for(ready); return child
        process=start(data)
        _,info=request('/api/v1/server-info')
        devices=[root/'device-a',root/'device-b']
        for device in devices: run(client,'--test-login',url,device,'test-user',cert)
        text=root/'body.txt'; text.write_text('在线快照内的正文和完整附件 🎈',encoding='utf-8')
        source=root/'原始附件.bin'; file_size=(4*1024*1024*1024 if args.large else 3*1024*1024)+17
        block=os.urandom(1024*1024); expected=hashlib.sha256()
        if args.resume_case:
            assert source.stat().st_size==file_size
            with source.open('rb') as stream:
                while chunk:=stream.read(1024*1024): expected.update(chunk)
        else:
            with source.open('wb') as output:
                for _ in range(file_size//len(block)): output.write(block); expected.update(block)
                output.write(block[:17]); expected.update(block[:17])
        def digest(path):
            h=hashlib.sha256()
            with path.open('rb') as stream:
                while chunk:=stream.read(1024*1024): h.update(chunk)
            return h.digest()
        transfer_start=time.monotonic()
        if not args.resume_case: run(client,'client-attach',devices[0],text,'self',source)
        run(client,'client-send',devices[0]); run(client,'client-sync',devices[1])
        def db(device): return device/info['server_id']/next((device/info['server_id']).iterdir()).name/'client.db'
        with sqlite3.connect(db(devices[1])) as local:
            object_id=local.execute('SELECT object_id FROM downloads').fetchone()[0]
        file_b=db(devices[1]).parent/'files'/f'{object_id}.file'
        assert file_b.stat().st_size==file_size and digest(file_b)==expected.digest()
        report['file_bytes']=file_size
        report['send_and_receive_seconds']=time.monotonic()-transfer_start
        record('native stable copy, encrypted upload and complete verified receive across 64-bit file length')
        run(server,'backup-trigger','--data',data)
        if args.large:
            wait_for(lambda: list(backend.glob('*/objects/*/*.chunk')),seconds=300)
            incomplete=list(backend.glob('*/snapshot.db'))[0].parent
            assert not (incomplete/'complete.bin').exists()
            process.terminate(); process.wait(timeout=20); process=None
            process=start(data)
            run(server,'backup-trigger','--data',data)
            record('actual process termination during large encrypted backup leaves unpublished snapshot and resumes same durable task')
        wait_for(lambda: list(backend.glob('*/complete.bin')),seconds=1800 if args.large else 30)
        complete=max(backend.glob('*/complete.bin'),key=lambda path:path.stat().st_mtime_ns).parent
        if args.large: assert complete==incomplete
        assert (complete/'snapshot.db').read_bytes()[:16]!=b'SQLite format 3\0'
        assert not list(complete.glob('*.dpapi')) and not list(complete.glob('*.pem'))
        record('running HTTPS server publishes encrypted online snapshot and full immutable attachment manifest')
        text.write_text('快照之后的较新本机正文',encoding='utf-8')
        run(client,'client-queue',devices[0],text); run(client,'client-send',devices[0]); run(client,'client-sync',devices[1])
        with sqlite3.connect(db(devices[0])) as local: before=local.execute('SELECT count(*) FROM messages').fetchone()[0]
        process.terminate(); process.wait(timeout=20); process=None
        text.write_text('断线结果不确定的待发送正文',encoding='utf-8')
        run(client,'client-queue',devices[0],text)
        restored=root/'new-directory-restored'
        run(server,'--test-restore','--data',restored,'--backup',complete,'--package',package)
        assert (restored/'runtime-user.dpapi').is_file() and not (restored/'portable-recovery.bin').exists()
        process=start(restored)
        _,restored_info=request('/api/v1/server-info')
        assert restored_info['server_id']==info['server_id'] and restored_info['generation']!=info['generation']
        run(client,'client-send',devices[0]); run(client,'client-sync',devices[1])
        with sqlite3.connect(db(devices[0])) as local:
            assert local.execute('SELECT count(*) FROM messages').fetchone()[0]==before
            assert local.execute("SELECT count(*) FROM pending WHERE state='restore_review'").fetchone()[0]==1
            assert local.execute("SELECT count(*) FROM pending WHERE state='queued'").fetchone()[0]==0
        assert file_b.stat().st_size==file_size and digest(file_b)==expected.digest()
        record('fresh-directory restore uses portable package; new generation preserves newer client history and complete files without blind resend')
        run(client,'client-retry-reviewed',devices[0]); run(client,'client-send',devices[0])
        with sqlite3.connect(db(devices[0])) as local: assert local.execute("SELECT count(*) FROM pending WHERE state='restore_review'").fetchone()[0]==0
        record('explicit reviewed retry resumes pending task after generation reconciliation')
        process.terminate(); process.wait(timeout=20); process=None
        chunk=next(complete.glob('objects/*/*.chunk')); intact=chunk.read_bytes(); damaged=bytearray(intact); damaged[-1]^=1; chunk.write_bytes(damaged)
        assert run(server,'--test-restore','--data',root/'tamper-rejected','--backup',complete,'--package',package,success=False).returncode!=0
        assert not (root/'tamper-rejected'/'runtime-user.dpapi').exists()
        chunk.write_bytes(intact)
        record('actual backup chunk tamper fails authentication and never publishes runnable restored keys')
        report['result']='PASS'
    except Exception as error:
        report['failure']=str(error); raise
    finally:
        if process and process.poll() is None: process.terminate(); process.wait(timeout=20)
        log.close(); (root/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(f'Backup verification: {root / "results.json"}',flush=True)

if __name__=='__main__': main()
