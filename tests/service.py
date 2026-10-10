"""Isolated SCM lifecycle and LocalSystem machine-DPAPI verification."""
import argparse
import http.client
import json
import os
import pathlib
import socket
import ssl
import subprocess
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--configuration', default='Debug')
    args = parser.parse_args()
    exe = ROOT / f'build/bin/x64/{args.configuration}/dropserver.exe'
    root = ROOT / f'build/tests/service-{args.configuration}-{uuid.uuid4().hex}'
    root.mkdir(parents=True)
    data = root / 'encrypted-server'
    name = f'DropTest-{uuid.uuid4().hex[:16]}'
    report = {'service': name, 'checks': [], 'result': 'FAIL'}
    installed = False
    def run(*arguments):
        result = subprocess.run([str(exe), *map(str, arguments)], capture_output=True, timeout=90)
        if result.returncode:
            safe = result.stderr.decode('utf-8', errors='replace')
            raise RuntimeError(f'{arguments[0]} failed: {safe[-500:]}')
        return result
    def record(text):
        report['checks'].append({'name': text, 'result': 'PASS'})
        print(f'PASS {text}', flush=True)
    try:
        run('--self-test', root)
        openssl = ROOT / 'third_party/installed-x64/x64-windows/tools/openssl/openssl.exe'
        env = os.environ.copy()
        env['OPENSSL_CONF'] = str(openssl.parent / 'openssl.cnf')
        key, cert, combined = data/'service-key.pem', data/'service-cert.pem', data/'service.pem'
        subprocess.run([str(openssl), 'req', '-x509', '-newkey', 'rsa:2048', '-sha256', '-nodes', '-days', '2', '-keyout', str(key), '-out', str(cert), '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost'], env=env, capture_output=True, check=True, timeout=60)
        combined.write_bytes(cert.read_bytes()+key.read_bytes())
        run('runtime-machine', '--data', data)
        assert (data/'runtime-machine.dpapi').is_file() and not (data/'runtime-user.dpapi').exists()
        record('controlled runtime migration removes user package and protects machine keys')
        permissions_env=os.environ.copy(); permissions_env['DROP_SERVICE_TEST_DIRECTORY']=str(data)
        permissions=subprocess.run([os.environ.get('DROP_SERVICE_POWERSHELL','pwsh.exe'),'-NoProfile','-Command',"$ErrorActionPreference='Stop'; $items=@(Get-Item -LiteralPath $env:DROP_SERVICE_TEST_DIRECTORY)+@(Get-ChildItem -LiteralPath $env:DROP_SERVICE_TEST_DIRECTORY -Recurse -Force); foreach($item in $items) { $acl=Get-Acl -LiteralPath $item.FullName; if(!$acl.AreAccessRulesProtected -or $acl.GetOwner([System.Security.Principal.SecurityIdentifier]).Value -ne 'S-1-5-32-544') { throw 'Unprotected directory owner' }; foreach($rule in $acl.GetAccessRules($true,$true,[System.Security.Principal.SecurityIdentifier])) { if($rule.IdentityReference.Value -notin @('S-1-5-18','S-1-5-32-544')) { throw 'Unexpected principal' } } }; Write-Output 'PASS protected tree'"],capture_output=True,env=permissions_env,timeout=30)
        assert permissions.returncode==0, 'Machine data tree permission/owner verification failed: '+permissions.stderr.decode(errors='replace')[-1000:]
        record('actual complete data tree has protected SYSTEM/Administrators ACL and Administrators owner without original-user ACE')
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0))
            port = listener.getsockname()[1]
        run('service-install','--name',name,'--data',data,'--certificate',combined,'--listen',f'127.0.0.1:{port}')
        installed = True
        run('service-start','--name',name)
        context = ssl.create_default_context(cafile=str(cert))
        def request(path, body=None, token=None):
            connection = http.client.HTTPSConnection('localhost',port,context=context,timeout=10)
            headers = {'Content-Type':'application/json'}
            if token:
                headers['Authorization'] = f'Bearer {token}'
            connection.request('POST' if body is not None else 'GET',path,json.dumps(body) if body is not None else None,headers)
            response = connection.getresponse()
            status, data = response.status,json.loads(response.read())
            connection.close()
            return status,data
        status,info = request('/api/v1/server-info')
        assert status == 200
        status,login = request('/api/v1/login',{'login':'test-user','password':'test-only-password-123','device_id':uuid.uuid4().hex,'device_name':'服务验收设备'})
        assert status == 200
        record('actual LocalSystem service starts HTTPS and unlocks machine-DPAPI SQLCipher')
        run('service-stop','--name',name)
        run('service-start','--name',name)
        status,restarted = request('/api/v1/server-info')
        assert status == 200 and info == restarted
        assert request('/api/v1/devices',token=login['token'])[0] == 200
        record('SCM stop and restart preserve encrypted identity and committed device credential')
        run('service-uninstall','--name',name)
        installed = False
        assert subprocess.run([str(exe),'service-status','--name',name],capture_output=True).returncode != 0
        record('custom-name service removed without touching other services')
        report['result'] = 'PASS'
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        if installed:
            subprocess.run([str(exe),'service-uninstall','--name',name],capture_output=True,timeout=90)
        (root/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print(f'Service verification: {root / "results.json"}',flush=True)

if __name__ == '__main__':
    main()
