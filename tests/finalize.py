"""Native background verifier: message isolation, interrupted validation and corruption."""
import argparse,hashlib,http.client,json,os,pathlib,socket,ssl,subprocess,time,uuid
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--configuration',default='Release');args=parser.parse_args()
    exe=ROOT/f'build/bin/x64/{args.configuration}/dropserver.exe';root=ROOT/f'build/tests/finalize-{args.configuration}-{uuid.uuid4().hex}';root.mkdir(parents=True)
    data=root/'encrypted-server';report={'result':'FAIL','checks':[]};process=None
    def run(*args): subprocess.run([str(exe),*map(str,args)],capture_output=True,check=True,timeout=120)
    def record(name): report['checks'].append({'name':name,'result':'PASS'});print('PASS '+name,flush=True)
    def wait(test):
        end=time.monotonic()+30
        while time.monotonic()<end:
            if test():return
            time.sleep(.1)
        raise RuntimeError('Native finalize condition timed out')
    log=(root/'server.log').open('wb')
    try:
        run('--self-test',root)
        openssl=ROOT/'third_party/installed-x64/x64-windows/tools/openssl/openssl.exe';key,cert,pem=root/'key.pem',root/'cert.pem',root/'tls.pem';env=os.environ.copy();env['OPENSSL_CONF']=str(openssl.parent/'openssl.cnf')
        subprocess.run([str(openssl),'req','-x509','-newkey','rsa:2048','-sha256','-nodes','-days','2','-keyout',str(key),'-out',str(cert),'-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],capture_output=True,check=True,env=env,timeout=60);pem.write_bytes(cert.read_bytes()+key.read_bytes())
        with socket.socket() as listener:listener.bind(('127.0.0.1',0));port=listener.getsockname()[1]
        context=ssl.create_default_context(cafile=str(cert))
        def request(method,path,body=None,token=None,binary=False):
            connection=http.client.HTTPSConnection('localhost',port,context=context,timeout=10);headers={'Content-Type':'application/octet-stream' if binary else 'application/json'}
            if token:headers['Authorization']='Bearer '+token
            connection.request(method,path,body if binary else json.dumps(body).encode() if body is not None else None,headers);response=connection.getresponse();raw=response.read();status=response.status;connection.close();return status,raw if binary else json.loads(raw)
        def start(pause=0):
            env=os.environ.copy();env['DROP_TEST_FINALIZE_PAUSE_MS']=str(pause)
            child=subprocess.Popen([str(exe),'serve','--data',str(data),'--certificate',str(pem),'--listen',f'127.0.0.1:{port}'],stdout=log,stderr=log,env=env)
            def ready():
                if child.poll() is not None:raise RuntimeError('Native server exited')
                try:return request('GET','/api/v1/server-info')[0]==200
                except OSError:return False
            wait(ready);return child
        process=start(10000)
        status,login=request('POST','/api/v1/login',{'login':'test-user','password':'test-only-password-123','device_id':uuid.uuid4().hex,'device_name':'async-finalize'});assert status==200;token=login['token']
        block=os.urandom(1048576);hash_state=hashlib.sha256()
        for _ in range(64):hash_state.update(block)
        def upload():
            status,info=request('POST','/api/v1/uploads',{'request_id':uuid.uuid4().hex,'filename':'后台校验.bin','total_size':str(64*1048576),'sha256':hash_state.hexdigest()},token);assert status==200;object_id=info['object_id']
            for i in range(64):assert request('PUT',f'/api/v1/uploads/{object_id}/chunks/{i}',block,token,True)[0]==200
            return object_id
        object_id=upload();start_time=time.monotonic();status,info=request('POST',f'/api/v1/uploads/{object_id}/complete',token=token)
        assert status==200 and info['state']=='validating' and time.monotonic()-start_time<3
        status,_=request('POST','/api/v1/messages',{'request_id':uuid.uuid4().hex,'body':'尚未校验不能接收','attachments':[object_id]},token);assert status==409
        status,_=request('POST','/api/v1/messages',{'request_id':uuid.uuid4().hex,'body':'校验期间仍可提交正文'},token);assert status==200
        assert request('GET',f'/api/v1/uploads/{object_id}',token=token)[1]['state']=='validating'
        record('real background validating returns promptly, rejects attachment publication and permits concurrent text commit')
        process.terminate();process.wait(timeout=10);process=start()
        status,info=request('GET',f'/api/v1/uploads/{object_id}',token=token);assert status==200 and info['state']=='uploading' and info['finalize_error']=='interrupted' and info['received_bitmap']=='ff'*8
        assert request('POST',f'/api/v1/uploads/{object_id}/complete',token=token)[0]==200
        wait(lambda:request('GET',f'/api/v1/uploads/{object_id}',token=token)[1]['state']=='complete')
        assert request('POST','/api/v1/messages',{'request_id':uuid.uuid4().hex,'body':'校验完成才接收','attachments':[object_id]},token)[0]==200
        record('actual process termination during validation preserves all 64 chunks and restart revalidates before message acceptance')
        other=upload();chunk=data/'objects'/other/'0000000000000000.chunk';cipher=bytearray(chunk.read_bytes());cipher[-1]^=1;chunk.write_bytes(cipher)
        assert request('POST',f'/api/v1/uploads/{other}/complete',token=token)[0]==200
        wait(lambda:request('GET',f'/api/v1/uploads/{other}',token=token)[1]['finalize_error']=='crypto_error')
        assert request('POST','/api/v1/messages',{'request_id':uuid.uuid4().hex,'body':'损坏附件不可接收','attachments':[other]},token)[0]==409
        assert request('PUT',f'/api/v1/uploads/{other}/chunks/0',block,token,True)[0]==200
        assert request('POST',f'/api/v1/uploads/{other}/complete',token=token)[0]==200
        wait(lambda:request('GET',f'/api/v1/uploads/{other}',token=token)[1]['state']=='complete')
        record('background cipher tamper persists crypto failure without false completion; explicit authenticated original-chunk repair succeeds')
        report['result']='PASS'
    except Exception as error:report['failure']=str(error);raise
    finally:
        if process and process.poll() is None:process.terminate();process.wait(timeout=10)
        log.close();(root/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');print(f'Finalize verification: {root / "results.json"}',flush=True)
if __name__=='__main__':main()
