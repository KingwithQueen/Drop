"""Native Drive backend over isolated TLS: faults, durable resume and authenticated recovery.

No Google account is contacted. Test refresh/access values remain isolated fixtures.
"""
import argparse
import hashlib
import http.client
import http.server
import json
import os
import pathlib
import re
import socket
import ssl
import subprocess
import threading
import time
import urllib.parse
import uuid
import sqlite3
from gui import Gui,wait_for

ROOT=pathlib.Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--configuration',default='Release'); args=parser.parse_args()
    binary=ROOT/f'build/bin/x64/{args.configuration}'; exe=binary/'dropserver.exe'; client=binary/'drop.exe'
    root=ROOT/f'build/tests/drive-{args.configuration}-{uuid.uuid4().hex}'; root.mkdir(parents=True)
    report={'result':'FAIL','real_google_account':False,'checks':[]}; mock=None; native=None; upload=None; ui=None
    state={'ids':0,'sessions':{},'files':{},'fault':None,'expiry':False,'lost':False,'ranges':[],'accepted':threading.Event(),'release':threading.Event(),'pause':False}
    def record(name): report['checks'].append({'name':name,'result':'PASS'}); print('PASS '+name,flush=True)
    def run(*arguments,success=True):
        result=subprocess.run([str(exe),*map(str,arguments)],capture_output=True,timeout=120)
        if success and result.returncode: raise RuntimeError(f'Native {arguments[0]} failed: {result.stdout.decode(errors="replace")[-160:]}')
        return result
    class Server(http.server.ThreadingHTTPServer):
        def handle_error(self,request,address):
            import sys
            if isinstance(sys.exception(),(ssl.SSLEOFError,ConnectionResetError,BrokenPipeError)): return
            super().handle_error(request,address)
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version='HTTP/1.1'
        def log_message(self,*args): pass
        def reply(self,status,body=None,headers=None):
            raw=json.dumps(body).encode() if body is not None else b''
            self.send_response(status); self.send_header('Content-Length',str(len(raw))); self.send_header('Content-Type','application/json')
            for key,value in (headers or {}).items(): self.send_header(key,value)
            self.end_headers()
            if raw: self.wfile.write(raw)
        def media(self,content):
            begin,end=map(int,re.fullmatch(r'bytes=(\d+)-(\d+)',self.headers['Range']).groups()); raw=content[begin:end+1]
            self.send_response(206); self.send_header('Content-Length',str(len(raw))); self.send_header('Content-Type','application/octet-stream'); self.end_headers(); self.wfile.write(raw)
        def handle_request(self):
            request=urllib.parse.urlparse(self.path); path=request.path; query=urllib.parse.parse_qs(request.query)
            body=self.rfile.read(int(self.headers.get('Content-Length','0')))
            if path=='/token':
                form=urllib.parse.parse_qs(body.decode()); assert form['grant_type']==['refresh_token']
                if state['fault']=='auth': return self.reply(400,{'error':'invalid_grant'})
                return self.reply(200,{'access_token':'isolated-drive-access','token_type':'Bearer','expires_in':3600})
            assert self.headers.get('Authorization')=='Bearer isolated-drive-access'
            if path=='/drive/v3/files/generateIds':
                assert query['space']==['appDataFolder']; state['ids']+=1; return self.reply(200,{'ids':[f'file{state["ids"]}']})
            if path=='/upload/drive/v3/files':
                if state['fault']=='quota':
                    state['fault']=None; return self.reply(403,{'error':{'errors':[{'reason':'storageQuotaExceeded'}]}})
                meta=json.loads(body); assert meta['parents']==['appDataFolder'] and meta['name']==meta['id']
                entry=meta['appProperties']['entry']; assert entry in ('snapshot.db','complete.bin') or re.fullmatch(r'objects/[a-f0-9]{32}/(manifest.bin|[a-f0-9]{16}\.chunk)',entry)
                key=f'session{len(state["sessions"])+1}'; state['sessions'][key]={'meta':meta,'total':int(self.headers['X-Upload-Content-Length']),'bytes':bytearray()}
                return self.reply(200,{}, {'Location':f'https://localhost:{mock.server_port}/upload/{key}'})
            if path.startswith('/upload/session'):
                key=path.rsplit('/',1)[1]; item=state['sessions'].get(key)
                if not item: return self.reply(404,{'error':'expired'})
                if not body:
                    if state['expiry']:
                        state['expiry']=False; del state['sessions'][key]; return self.reply(404,{'error':'expired'})
                    if item['meta']['id'] in state['files']: return self.reply(200,{'id':item['meta']['id']})
                    return self.reply(308,headers={'Range':f'bytes=0-{len(item["bytes"])-1}'} if item['bytes'] else {})
                if state['fault']=='rate':
                    state['fault']=None; return self.reply(429,{'error':'rateLimit'})
                begin,end,total=map(int,re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)',self.headers['Content-Range']).groups())
                assert total==item['total'] and begin==len(item['bytes']) and len(body)==end-begin+1
                assert begin%262144==0 and (len(body)%262144==0 or end+1==total)
                item['bytes'].extend(body); state['ranges'].append([begin,end,total])
                if state['pause']:
                    state['pause']=False; state['accepted'].set(); state['release'].wait(30)
                if end+1==total:
                    state['files'][item['meta']['id']]={**item['meta'],'size':str(total),'data':bytes(item['bytes'])}
                    if state['lost']:
                        state['lost']=False; self.close_connection=True; self.connection.shutdown(socket.SHUT_RDWR); return
                    return self.reply(200,{'id':item['meta']['id'],'size':str(total)})
                return self.reply(308,headers={'Range':f'bytes=0-{end}'})
            if path=='/drive/v3/files':
                q=query.get('q',[''])[0]; match=re.search(r"key='dropBackup' and value='([a-f0-9]{32})'",q); marker="key='complete' and value='1'" in q
                items=[{key:value for key,value in item.items() if key in ('id','size','appProperties')} for item in state['files'].values() if (not match or item['appProperties']['dropBackup']==match.group(1)) and (not marker or item['appProperties']['complete']=='1')]
                page=int(query.get('pageToken',['0'])[0]); response={'files':items[page:page+50]}
                if page+50<len(items): response['nextPageToken']=str(page+50)
                return self.reply(200,response)
            if path.startswith('/drive/v3/files/'):
                key=path.rsplit('/',1)[1]; item=state['files'].get(key)
                if not item: return self.reply(404,{'error':'notFound'})
                if self.command=='DELETE': del state['files'][key]; return self.reply(204)
                if query.get('alt')==['media']: return self.media(item['data'])
                return self.reply(200,{key:value for key,value in item.items() if key in ('id','size','appProperties')})
            self.reply(404,{'error':'unknown route'})
        def do_GET(self): self.handle_request()
        def do_POST(self): self.handle_request()
        def do_PUT(self): self.handle_request()
        def do_DELETE(self): self.handle_request()
    log=(root/'native-server.log').open('wb')
    try:
        run('--test-backup',root)
        data=next(p for p in root.glob('backup-test-*') if (p/'portable-recovery.bin').is_file())
        backup=next((data/'local-backups').iterdir()); task=backup.name; package=data/'portable-recovery.bin'
        openssl=ROOT/'third_party/installed-x64/x64-windows/tools/openssl/openssl.exe'; key,cert,pem=root/'key.pem',root/'cert.pem',root/'tls.pem'
        env=os.environ.copy(); env['OPENSSL_CONF']=str(openssl.parent/'openssl.cnf')
        subprocess.run([str(openssl),'req','-x509','-newkey','rsa:2048','-sha256','-nodes','-days','2','-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],capture_output=True,check=True,env=env,timeout=60); pem.write_bytes(cert.read_bytes()+key.read_bytes())
        mock=Server(('127.0.0.1',0),Handler); tls=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); tls.load_cert_chain(cert,key); mock.socket=tls.wrap_socket(mock.socket,server_side=True); threading.Thread(target=mock.serve_forever,daemon=True).start()
        origin=f'https://localhost:{mock.server_port}'
        run('--test-drive-authorize','--data',data,'--origin',origin,'--ca',cert)
        secret=(data/'drive-user.dpapi').read_bytes(); assert b'isolated-drive-refresh-fixture' not in secret
        record('native user-DPAPI protects cloud refresh credentials; mock origin exists only in diagnostic builds')
        state['fault']='auth'; result=run('drive-upload','--data',data,'--backup',backup,success=False); assert result.returncode and b'unauthorized' in result.stdout; state['fault']=None
        record('expired refresh authorization fails explicitly without publishing backup')
        state['fault']='quota'; result=run('drive-upload','--data',data,'--backup',backup,success=False); assert result.returncode and b'space_unavailable' in result.stdout
        state['fault']='rate'; result=run('drive-upload','--data',data,'--backup',backup,success=False); assert result.returncode and b'network_error' in result.stdout
        assert not any(item['appProperties']['complete']=='1' for item in state['files'].values())
        record('native quota and rate-limit failures persist retry state; incomplete backup is not listed')
        state['pause']=True
        upload=subprocess.Popen([str(exe),'drive-upload','--data',str(data),'--backup',str(backup)],stdout=subprocess.DEVNULL,stderr=log)
        assert state['accepted'].wait(30); upload.terminate(); upload.wait(timeout=10); upload=None; state['release'].set(); time.sleep(.2)
        record('actual native uploader termination after accepted bytes leaves resumable encrypted queue')
        state['expiry']=True; state['lost']=True
        run('drive-upload','--data',data,'--backup',backup,success=False)
        run('drive-upload','--data',data,'--backup',backup)
        local={p.relative_to(backup).as_posix():p.read_bytes() for p in backup.rglob('*') if p.is_file()}
        remote={p['appProperties']['entry']:p['data'] for p in state['files'].values()}
        assert local==remote and len(remote)==len(state['files'])
        assert all(not content.startswith(b'SQLite format 3\0') for content in remote.values())
        listing=run('drive-list','--data',data); assert task.encode() in listing.stdout
        original_marker=next(item for item in state['files'].values() if item['appProperties']['complete']=='1')
        synthetic=[]
        for index in range(55):
            identifier=f'pagination{index}'; synthetic.append(identifier)
            state['files'][identifier]={**original_marker,'id':identifier,'appProperties':{**original_marker['appProperties'],'dropBackup':uuid.uuid4().hex}}
        assert run('drive-list','--data',data).stdout.count(b'backup=')==56
        for identifier in synthetic: del state['files'][identifier]
        record('native appData listing follows actual 50-row page tokens and preserves all 56 catalog entries')
        status=run('drive-status','--data',data); assert b'state=complete' in status.stdout
        record('expired resumable session and lost final reply recover with preallocated IDs; cloud holds exact ciphertext and publishes manifest last')
        target=root/'downloaded-ciphertext'; run('drive-download','--data',data,'--backup-id',task,'--target',target)
        assert local=={p.relative_to(target).as_posix():p.read_bytes() for p in target.rglob('*') if p.is_file()}
        restored=root/'restored'; run('--test-restore','--data',restored,'--backup',target,'--package',package)
        assert (restored/'runtime-user.dpapi').is_file()
        record('native HTTPS cloud download and portable-package authenticated restore succeed in fresh directory')
        state['files']['pathInjection']={**original_marker,'id':'pathInjection','appProperties':{**original_marker['appProperties'],'entry':'../escape.txt','complete':'0'}}
        result=run('drive-download','--data',data,'--backup-id',task,'--target',root/'path-rejected',success=False)
        assert result.returncode and not (root/'path-rejected').exists() and not (root/'escape.txt').exists()
        del state['files']['pathInjection']
        record('native cloud metadata path traversal rejected before creating download target')
        # Complete cloud recovery is then read through the real encrypted HTTPS server.
        with socket.socket() as listener: listener.bind(('127.0.0.1',0)); port=listener.getsockname()[1]
        native=subprocess.Popen([str(exe),'serve','--data',str(restored),'--certificate',str(pem),'--listen',f'127.0.0.1:{port}'],stdout=log,stderr=log)
        context=ssl.create_default_context(cafile=str(cert))
        def request(path,body=None,token=None):
            connection=http.client.HTTPSConnection('localhost',port,context=context,timeout=10); headers={'Content-Type':'application/json'}
            if token: headers['Authorization']='Bearer '+token
            connection.request('POST' if body is not None else 'GET',path,json.dumps(body).encode() if body is not None else None,headers); response=connection.getresponse(); raw=response.read(); code=response.status; connection.close(); return code,raw
        deadline=time.monotonic()+30
        while True:
            try:
                code,raw=request('/api/v1/server-info'); info=json.loads(raw); assert code==200; break
            except OSError:
                if time.monotonic()>deadline: raise
                time.sleep(.1)
        code,raw=request('/api/v1/login',{'login':'backup-user','password':'backup-fixture-password-123','device_id':uuid.uuid4().hex,'device_name':'cloud-restore-check'}); assert code==200; token=json.loads(raw)['token']
        code,raw=request('/api/v1/sync',{'generation':info['generation'],'after':'0','limit':50},token); assert code==200; message=json.loads(raw)['events'][0]['message']; assert message['body']=='加密恢复正文 🎈'
        attachment=message['attachments'][0]; digest=hashlib.sha256(); total=0
        for index in range((int(attachment['total_size'])+1048575)//1048576):
            code,raw=request(f'/api/v1/attachments/{attachment["object_id"]}/chunks/{index}',token=token); assert code==200; digest.update(raw); total+=len(raw)
        assert total==int(attachment['total_size']) and digest.hexdigest()==attachment['sha256']
        record('real native restored server decrypts original Unicode history and full SHA-256-verified attachment')
        ui=Gui(client,root/'cloud-restored-client')
        ui.set(100,f'https://localhost:{port}'); ui.set(101,str(cert)); ui.set(102,'backup-user'); ui.set(103,'backup-fixture-password-123'); ui.click(105); ui.idle()
        def native_received():
            databases=list((root/'cloud-restored-client').glob('*/*/client.db'))
            if not databases: return False
            with sqlite3.connect(databases[0]) as db:
                row=db.execute('SELECT object_id,state FROM downloads').fetchone()
            return row and row[1]=='complete' and hashlib.sha256((databases[0].parent/'files'/f'{row[0]}.file').read_bytes()).hexdigest()==attachment['sha256']
        wait_for(native_received,'Native cloud-restored attachment did not complete',45)
        ui.capture(root/'cloud-restored-client.png'); ui.click(126); ui.process.wait(timeout=10); ui=None
        record('actual native tray client receives restored cloud history and complete verified attachment without Google login')
        native.terminate(); native.wait(timeout=15); native=None
        chunk=next(p for p in state['files'].values() if p['appProperties']['entry'].endswith('.chunk')); damaged=bytearray(chunk['data']); damaged[-1]^=1; chunk['data']=bytes(damaged)
        result=run('drive-download','--data',data,'--backup-id',task,'--target',root/'tampered-download',success=False); assert result.returncode and b'crypto_error' in result.stdout
        assert not (root/'tampered-download'/'complete.bin').exists()
        record('damaged remote ciphertext fails native SHA-256 before complete marker publication')
        run('drive-delete','--data',data,'--backup-id',task); assert not state['files']; assert task.encode() not in run('drive-list','--data',data).stdout
        assert backup.is_dir() and local=={p.relative_to(backup).as_posix():p.read_bytes() for p in backup.rglob('*') if p.is_file()}
        record('explicit remote deletion removes completion marker and cloud files while preserving local encrypted backup')
        run('backup','--data',data,'--target',data/'local-backups')
        state['fault']='quota'
        native=subprocess.Popen([str(exe),'serve','--data',str(data),'--certificate',str(pem),'--listen',f'127.0.0.1:{port}','--backup-directory',str(data/'local-backups')],stdout=log,stderr=log)
        deadline=time.monotonic()+30
        while state['fault']:
            if time.monotonic()>deadline: raise RuntimeError('Background cloud worker did not attempt backup')
            time.sleep(.1)
        code,raw=request('/api/v1/login',{'login':'backup-user','password':'backup-fixture-password-123','device_id':uuid.uuid4().hex,'device_name':'cloud-failure-isolation'}); assert code==200; token=json.loads(raw)['token']
        code,raw=request('/api/v1/messages',{'request_id':uuid.uuid4().hex,'body':'云盘配额失败仍可接收正文'},token); assert code==200
        record('actual independent background cloud worker quota failure leaves native HTTPS message commit available')
        native.terminate(); native.wait(timeout=15); native=None
        report['result']='PASS'; report['range_requests']=len(state['ranges']); report['encrypted_files']=len(local)
    except Exception as error: report['failure']=str(error); raise
    finally:
        state['release'].set()
        if ui and ui.process.poll() is None: ui.click(126); ui.process.wait(timeout=10)
        for process in (upload,native):
            if process and process.poll() is None: process.terminate(); process.wait(timeout=15)
        if mock: mock.shutdown(); mock.server_close()
        log.close(); (root/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8'); print(f'Drive verification: {root / "results.json"}',flush=True)

if __name__=='__main__': main()
