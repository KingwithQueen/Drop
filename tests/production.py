"""Verify fixture-free production binaries with native masked administration and UI."""
import argparse
import json
import os
import pathlib
import secrets
import socket
import sqlite3
import subprocess
import time
import ctypes
import http.client
import ssl
import threading
import uuid
from ctypes import wintypes as w
from masked_console import masked_command
from gui import Gui,wait_for
from resource_sample import sample

ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--platform',default='x64'); parser.add_argument('--binary'); parser.add_argument('--stress',action='store_true')
    args=parser.parse_args(); binary=pathlib.Path(args.binary).resolve() if args.binary else ROOT/f'build/production/{args.platform}'
    server,client=binary/'dropserver.exe',binary/'drop.exe'
    root=ROOT/f'build/tests/production-{args.platform}-{secrets.token_hex(16)}'; root.mkdir(parents=True)
    data=root/'server'; account='release-user'; password=secrets.token_urlsafe(32)
    report={'platform':args.platform,'checks':[],'result':'FAIL'}; process=None; ui=None
    def run(exe,*arguments):
        result=subprocess.run([str(exe),*map(str,arguments)],stdin=subprocess.DEVNULL,capture_output=True,timeout=30)
        if result.returncode: raise RuntimeError(f'Production {arguments[0]} failed')
    def record(text):
        report['checks'].append({'name':text,'result':'PASS'}); print(f'PASS {text}',flush=True)
    log=(root/'server.log').open('wb')
    try:
        for exe in (server,client):
            content=exe.read_bytes()
            for secret in ('test-only-password-123','test-only-other-password-123','test-only-third-password-123','backup-fixture-password-123','migration-test-password-123','independent-recovery-fixture-password-123','isolated-drive-refresh-fixture','isolated-drive-fixture'):
                assert secret.encode() not in content and secret.encode('utf-16le') not in content
            assert subprocess.run([str(exe),'--self-test',str(root)],capture_output=True).returncode!=0
            assert subprocess.run([str(exe),'--test-login','https://localhost:1',str(root),'test-user'],capture_output=True).returncode!=0
        record('production binary excludes test passwords and rejects fixture entry points')
        run(server,'init','--data',data)
        masked_command([server,'account-create','--data',data,'--login',account],password)
        record('actual hidden native console disables password echo and creates account without secrets in arguments')
        triplet='x64-windows' if args.platform=='x64' else 'x86-windows'
        openssl=ROOT/f'third_party/installed-{args.platform}/{triplet}/tools/openssl/openssl.exe'
        key,cert,pem=data/'key.pem',data/'cert.pem',data/'tls.pem'
        env=os.environ.copy(); env['OPENSSL_CONF']=str(openssl.parent/'openssl.cnf')
        subprocess.run([str(openssl),'req','-x509','-newkey','rsa:2048','-nodes','-days','2','-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],stdin=subprocess.DEVNULL,capture_output=True,check=True,env=env,timeout=60)
        pem.write_bytes(cert.read_bytes()+key.read_bytes())
        with socket.socket() as listener: listener.bind(('127.0.0.1',0)); port=listener.getsockname()[1]
        process=subprocess.Popen([str(server),'serve','--data',str(data),'--certificate',str(pem),'--listen',f'127.0.0.1:{port}'],stdin=subprocess.DEVNULL,stdout=log,stderr=log)
        ui=Gui(client,root/'native-ui')
        ui.set(100,f'https://localhost:{port}'); ui.set(101,str(cert)); ui.set(102,account); ui.set(103,password); ui.click(105); ui.idle()
        wait_for(lambda:ui.enabled(109),'Production native login failed'); assert not ui.text(103)
        text='正式候选版本原生收发 🎈'; ui.set(107,text); ui.click(109); ui.idle()
        def received():
            paths=list((root/'native-ui').glob('*/*/client.db'))
            if not paths: return False
            with sqlite3.connect(paths[0]) as local: return local.execute('SELECT count(*) FROM messages').fetchone()[0]==1
        wait_for(received,'Production send/history synchronization failed')
        record('packaged production native UI logs in, clears password and durably sends/synchronizes Chinese body')
        report['idle_resources']=sample({'server':process,'client':ui.process})
        record('30-second logged-in native client and HTTPS server idle CPU/memory measured')
        if args.stress:
            load={'messages':0,'errors':[]}; context=ssl.create_default_context(cafile=str(cert))
            def produce():
                connection=http.client.HTTPSConnection('localhost',port,context=context,timeout=15); start=time.monotonic()
                try:
                    connection.request('POST','/api/v1/login',json.dumps({'login':account,'password':password,'device_id':uuid.uuid4().hex,'device_name':'load-producer'}),{'Content-Type':'application/json'})
                    response=connection.getresponse(); assert response.status==200; token=json.loads(response.read())['token']
                    for i in range(500):
                        connection.request('POST','/api/v1/messages',json.dumps({'request_id':uuid.uuid4().hex,'body':f'连续收发 {i} 🎈 '+('正文'*512)},ensure_ascii=False).encode(),{'Content-Type':'application/json','Authorization':'Bearer '+token})
                        response=connection.getresponse(); response.read(); assert response.status==200; load['messages']+=1
                        time.sleep(.03)
                except Exception as error: load['errors'].append(str(error))
                finally: connection.close(); load['seconds']=time.monotonic()-start
            producer=threading.Thread(target=produce); producer.start()
            report['load_resources']=sample({'server':process,'client':ui.process}); producer.join(timeout=30)
            assert not producer.is_alive() and not load['errors'] and load['messages']==500
            def caught_up():
                paths=list((root/'native-ui').glob('*/*/client.db'))
                with sqlite3.connect(paths[0]) as local: return local.execute('SELECT count(*) FROM messages').fetchone()[0]==501
            wait_for(caught_up,'Native client did not complete 500-message catch-up',60)
            report['continuous_load']=load
            record('500 actual production HTTPS commits and native tray catch-up with 30-second load CPU/memory measurement')
        ui.capture(root/'production-ui.png')
        ui.click(126); ui.process.wait(timeout=10); ui=None
        report['result']='PASS'
    except Exception as error: report['failure']=str(error); raise
    finally:
        password=None
        if ui and ui.process.poll() is None: ui.click(126); ui.process.wait(timeout=10)
        if process and process.poll() is None: process.terminate(); process.wait(timeout=20)
        log.close(); (root/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8'); print(f'Production verification: {root / "results.json"}',flush=True)

if __name__=='__main__': main()
