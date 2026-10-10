"""Real isolated HTTPS server tests. Never print test credentials or responses."""
import argparse
import datetime
import http.client
import hashlib
import json
import os
import pathlib
import socket
import sqlite3
import ssl
import subprocess
import time
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
REPORT_CONTEXT = None


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    global REPORT_CONTEXT
    parser = argparse.ArgumentParser()
    parser.add_argument('--platform', default='x64', choices=['x64', 'Win32'])
    parser.add_argument('--configuration', default='Debug', choices=['Debug', 'Release'])
    args = parser.parse_args()
    triplet = 'x64-windows' if args.platform == 'x64' else 'x86-windows'
    binary = ROOT / f'build/bin/{args.platform}/{args.configuration}'
    server_exe = binary / 'dropserver.exe'
    client_exe = binary / 'drop.exe'
    openssl = ROOT / f'third_party/installed-{args.platform}/{triplet}/tools/openssl/openssl.exe'
    run_root = ROOT / f'build/tests/integration-{args.platform}-{args.configuration}-中文-{uuid.uuid4().hex}'
    run_root.mkdir(parents=True)
    data_root = run_root / 'encrypted-server'
    report = {'platform': args.platform, 'configuration': args.configuration,
        'time': datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat(), 'checks': []}
    REPORT_CONTEXT = (run_root / 'results.json', report)

    def record(name):
        report['checks'].append({'name': name, 'result': 'PASS'})
        print(f'PASS {name}', flush=True)

    def run(command):
        result = subprocess.run(list(map(str, command)), capture_output=True, timeout=60)
        safe_status = [line for line in result.stdout.decode('utf-8', errors='replace').splitlines()
            if line.startswith(('Client:', 'Login:', 'Connection:'))]
        if result.returncode:
            safe_errors = [line for line in result.stderr.decode('utf-8', errors='replace').splitlines() if line.startswith('network_request_failed')]
            if safe_errors:
                print('\n'.join(safe_errors), flush=True)
        check(result.returncode == 0, f'{pathlib.Path(command[0]).name} operation failed (exit {result.returncode}; {safe_status[-1] if safe_status else "no diagnostic"})')
        return result

    run([client_exe, '--self-test', run_root])
    run([server_exe, '--self-test', run_root])
    record('native client/common and encrypted server self-tests')
    # The server initialized this protected directory before writing test certificates.
    key = data_root / 'test-key.pem'
    cert = data_root / 'test-cert.pem'
    combined = data_root / 'test-combined.pem'
    environment = os.environ.copy()
    environment['OPENSSL_CONF'] = str(openssl.parent / 'openssl.cnf')
    generated = subprocess.run([str(openssl), 'req', '-x509', '-newkey', 'rsa:2048', '-sha256',
        '-nodes', '-days', '2', '-keyout', str(key), '-out', str(cert), '-subj', '/CN=localhost',
        '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], env=environment, capture_output=True, timeout=60)
    check(generated.returncode == 0, 'Cannot generate isolated test certificate')
    combined.write_bytes(cert.read_bytes() + key.read_bytes())
    context = ssl.create_default_context(cafile=str(cert))
    if os.environ.get('DROP_TEST_TLS12') == '1':
        context.maximum_version = ssl.TLSVersion.TLSv1_2
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    log = (run_root / 'server-output.log').open('wb')
    drop_heartbeats = False
    server_config = None

    def start():
        server_environment = os.environ.copy()
        server_environment['DROP_TEST_TRACE'] = '1'
        if drop_heartbeats:
            server_environment['DROP_TEST_DROP_HEARTBEAT'] = '1'
        command = [str(server_exe), 'serve', '--data', str(data_root), '--listen', f'127.0.0.1:{port}', '--certificate', str(combined)]
        if server_config:
            command += ['--config', str(server_config)]
        process = subprocess.Popen(command,
            stdin=subprocess.DEVNULL, stdout=log, stderr=log,
            env=server_environment,
            creationflags=subprocess.CREATE_NO_WINDOW)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            check(process.poll() is None, 'HTTPS server exited during startup')
            try:
                connection = socket.create_connection(('127.0.0.1', port), timeout=0.25)
                connection.close()
                return process
            except OSError:
                time.sleep(0.1)
        process.terminate()
        process.wait(timeout=20)
        raise RuntimeError('HTTPS server startup timed out')

    def request(path, method='GET', payload=None, token=None, raw=None, close=False):
        connection = http.client.HTTPSConnection('127.0.0.1', port, context=context, timeout=15)
        headers = {'Content-Type': 'application/json'}
        if close:
            headers['Connection'] = 'close'
        if token:
            headers['Authorization'] = f'Bearer {token}'
        body = raw if raw is not None else json.dumps(payload, ensure_ascii=False).encode() if payload is not None else None
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def login(name, password, device):
        return request('/api/v1/login', 'POST', {'login': name, 'password': password,
            'device_id': device, 'device_name': '中文集成测试设备'})

    def binary_request(path, method='GET', data=None, credential=None):
        connection = http.client.HTTPSConnection('127.0.0.1', port, context=context, timeout=30)
        headers = {'Content-Type': 'application/octet-stream'}
        if credential:
            headers['Authorization'] = f'Bearer {credential}'
        try:
            connection.request(method, path, body=data, headers=headers)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    process = start()
    try:
        status, info = request('/api/v1/server-info')
        check(status == 200 and info['protocol_version'] == 1 and len(info['server_id']) == 32, 'Server info failed')
        record('trusted isolated HTTPS certificate and protocol v1')
        failures = 0
        for _ in range(100):
            try:
                code, _ = request('/api/v1/server-info', close=True)
                failures += int(code != 200)
            except (OSError, http.client.HTTPException):
                failures += 1
        check(failures == 0, f'HTTPS short-connection regression: {failures}/100 responses lost')
        record('HTTPS short-connection response regression 100/100')
        rejected = subprocess.run([str(client_exe), '--probe', f'https://localhost:{port}'], capture_output=True, timeout=40)
        check(rejected.returncode != 0 and b'certificate_error' in rejected.stdout, 'WinHTTP accepted untrusted certificate or failed for another reason')
        record('native WinHTTP rejects untrusted certificate')
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
                context.wrap_socket(sock, server_hostname='wrong.invalid').close()
        except ssl.SSLCertVerificationError:
            record('HTTPS hostname mismatch rejected')
        else:
            raise RuntimeError('Hostname mismatch accepted')
        status, _ = request('/api/v1/devices')
        check(status == 401, 'Unauthenticated device access accepted')
        status, _ = request('/api/v1/devices', token='0' * 64)
        check(status == 401, 'Invalid token accepted')
        status, _ = request('/server.db')
        check(status == 404, 'Static files exposed')
        record('unauthorized token and static-file access rejected')
        status, _ = request('/api/v1/login', 'POST', raw=b'{"login":"test-user","login":"test-other"}')
        check(status == 400, 'Duplicate JSON keys accepted')
        status, _ = request('/api/v1/login', 'POST', raw=b'{"login":"test-user\\u0000"}')
        check(status == 400, 'NUL JSON accepted')
        record('malformed protocol requests rejected')
        time.sleep(1.05)
        device = uuid.uuid4().hex
        status, _ = login('test-user', 'incorrect-test-password', device)
        check(status == 401, 'Wrong password accepted')
        status, response = login('test-user', 'test-only-password-123', device)
        check(status == 200, 'Test account login failed')
        token = response['token']
        status, devices = request('/api/v1/devices', token=token)
        check(status == 200 and any(item['device_id'] == device and item['name'] == '中文集成测试设备' for item in devices['devices']), 'Device persisted incorrectly')
        record('Argon2id login and UTF-8 device persistence')
        other_device = uuid.uuid4().hex
        status, other = login('test-other', 'test-only-other-password-123', other_device)
        check(status == 200, 'Other account login failed')
        status, other_devices = request('/api/v1/devices', token=other['token'])
        check(status == 200 and all(item['device_id'] != device for item in other_devices['devices']), 'Cross-account device data exposed')
        record('account device isolation')
        other_login = other
        # Stage 2 REST: default self, durable idempotency, two-party events and isolation.
        payload = {'request_id': uuid.uuid4().hex, 'body': '阶段二中文 🎈 self'}
        code, self_message = request('/api/v1/messages', 'POST', payload, token)
        check(code == 200 and not self_message['duplicate'], 'Default self submit failed')
        code, replay = request('/api/v1/messages', 'POST', payload, token)
        check(code == 200 and replay['duplicate'] and replay['message_id'] == self_message['message_id'], 'Idempotency failed')
        code, _ = request('/api/v1/messages', 'POST', dict(payload, body='different'), token)
        check(code == 409, 'Changed duplicate content accepted')
        code, lookup = request('/api/v1/accounts', 'POST', {'login': 'test-other'}, token)
        check(code == 200 and lookup['user_id'] == other_login['user_id'], 'Recipient lookup failed')
        payload_other = {'request_id': uuid.uuid4().hex, 'body': '双方消息', 'recipient_user_id': other_login['user_id']}
        code, sent = request('/api/v1/messages', 'POST', payload_other, token)
        check(code == 200, 'Two-party submit failed')
        sync_input = {'generation': info['generation'], 'after': '0', 'limit': 1}
        code, first = request('/api/v1/sync', 'POST', sync_input, token)
        check(code == 200 and first['next'] == '1' and first['more'] and len(first['events']) == 1, 'First sync page failed')
        code, second = request('/api/v1/sync', 'POST', dict(sync_input, after='1'), token)
        check(code == 200 and second['next'] == '2' and not second['more'], 'Second sync page failed')
        code, received = request('/api/v1/sync', 'POST', dict(sync_input, limit=100), other_login['token'])
        check(code == 200 and len(received['events']) == 1 and received['events'][0]['message']['message_id'] == sent['message_id'], 'Recipient events or isolation failed')
        code, _ = request('/api/v1/sync', 'POST', dict(sync_input, generation=uuid.uuid4().hex), token)
        check(code == 409, 'Stale generation accepted')
        code, _ = request('/api/v1/sync', 'POST', dict(sync_input, after='9999999'), token)
        check(code == 409, 'Impossible cursor accepted')
        code, _ = request('/api/v1/sync', 'POST', dict(sync_input, after='01'), token)
        check(code == 400, 'Noncanonical cursor accepted')
        record('default-self and two-party text, idempotency conflict, pagination, generation and account isolation')

        if True:  # Application-owned CA, without system trust changes.
            url = f'https://localhost:{port}'
            wrong_ca = data_root / 'wrong-ca.pem'
            wrong_key = data_root / 'wrong-ca-key.pem'
            created_ca = subprocess.run([str(openssl), 'req', '-x509', '-newkey', 'rsa:2048', '-sha256',
                '-nodes', '-days', '2', '-keyout', str(wrong_key), '-out', str(wrong_ca), '-subj', '/CN=wrong-root'],
                env=environment, capture_output=True, timeout=60)
            check(created_ca.returncode == 0, 'Cannot generate negative private-CA fixture')
            bad_root = run_root / 'native-rejected-ca'
            bad_login = subprocess.run([str(client_exe), '--test-login', url, str(bad_root), 'test-user', str(wrong_ca)],
                capture_output=True, timeout=45)
            check(bad_login.returncode != 0 and not (bad_root / 'session.dpapi').exists(), 'Unrelated private CA accepted')
            bad_device = (bad_root / 'device.id').read_text(encoding='ascii')
            code, current_devices = request('/api/v1/devices', token=token)
            check(code == 200 and all(item['device_id'] != bad_device for item in current_devices['devices']),
                'Rejected-CA login reached server authentication; credentials may have been transmitted')
            record('unrelated private CA rejected before credential submission')
            devices = [run_root / 'native-A', run_root / 'native-B', run_root / 'native-other']
            time.sleep(1.1)
            for directory, account in zip(devices, ['test-user', 'test-user', 'test-other']):
                run([client_exe, '--test-login', url, directory, account, cert])
                run([client_exe, 'client-sync', directory])

            def local_db(directory):
                paths = list(directory.glob('*/*/client.db'))
                check(len(paths) == 1, 'Client state is not separated by server and account')
                return paths[0]

            def count(directory):
                with sqlite3.connect(local_db(directory)) as local:
                    return local.execute('SELECT count(*) FROM messages').fetchone()[0]

            check([count(p) for p in devices] == [2, 2, 1], 'Native history bootstrap mismatch')
            text_file = run_root / '中文正文.txt'
            text_file.write_text('原生客户端自发消息 🎈', encoding='utf-8')
            run([client_exe, 'client-queue', devices[0], text_file])
            # The process restarts between queueing and sending, proving persistence.
            run([client_exe, 'client-send', devices[0]])
            # Simulate loss of the server response before local acceptance was stored.
            with sqlite3.connect(local_db(devices[0])) as local:
                local.execute("UPDATE pending SET state='queued',message_id=NULL")
            run([client_exe, 'client-send', devices[0]])
            run([client_exe, 'client-sync', devices[1]])
            check(count(devices[0]) == 3 and count(devices[1]) == 3 and count(devices[2]) == 1, 'Durable retry duplicated messages or leaked self message')
            record('two native clients, DPAPI session, persistent queue and lost-acceptance idempotent replay')
            # A lost save-confirmation response must not append duplicate state events.
            with sqlite3.connect(local_db(devices[1])) as local:
                ack = local.execute("SELECT value FROM meta WHERE key='saved_ack_cursor'").fetchone()[0]
                local.execute("UPDATE meta SET value='0' WHERE key='saved_ack_cursor'")
            code, before_states = request('/api/v1/states', 'POST', {'generation': info['generation'], 'after': '0'}, token)
            check(code == 200, 'State stream failed')
            run([client_exe, 'client-sync', devices[1]])
            code, after_states = request('/api/v1/states', 'POST', {'generation': info['generation'], 'after': '0'}, token)
            check(code == 200 and before_states['next'] == after_states['next'], 'Duplicate body acknowledgement generated duplicate state')
            with sqlite3.connect(local_db(devices[1])) as local:
                check(local.execute("SELECT value FROM meta WHERE key='saved_ack_cursor'").fetchone()[0] == ack, 'Lost acknowledgement did not recover')
                selected_message = local.execute('SELECT message_id FROM messages ORDER BY rowid DESC LIMIT 1').fetchone()[0]
            run([client_exe, 'client-read', devices[1], selected_message])
            run([client_exe, 'client-sync', devices[0]])
            with sqlite3.connect(local_db(devices[0])) as local:
                check(local.execute('SELECT count(*) FROM account_reads').fetchone()[0] > 0, 'Account read position did not reach another device')
            code, _ = request('/api/v1/read', 'POST', {'generation': info['generation'], 'conversation_id': first['events'][0]['message']['conversation_id'], 'message_id': self_message['message_id']}, other_login['token'])
            check(code == 401, 'Another account could mark a private self conversation read')
            code, _ = request('/api/v1/saved', 'POST', {'generation': info['generation'], 'body_after': '9999999', 'objects': []}, token)
            check(code == 409, 'Save position could exceed durable history')
            record('body saved receipts, lost-confirmation idempotency, account read replication and receipt permissions')

            watcher = subprocess.Popen([str(client_exe), 'client-watch', str(devices[1])], stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                check(watcher.stdout.readline().strip() == b'Watch: ready', 'Native WSS did not connect')
                text_file.write_text('原生 WSS 及时补齐', encoding='utf-8')
                run([client_exe, 'client-queue', devices[0], text_file, other_login['user_id']])
                run([client_exe, 'client-send', devices[0]])
                watched, errors = watcher.communicate(timeout=30)
                check(watcher.returncode == 0 and b'Watch: synchronized' in watched, 'Native WSS notification/sync failed')
            finally:
                if watcher.poll() is None:
                    watcher.terminate()
                    watcher.wait(timeout=20)
            run([client_exe, 'client-sync', devices[2]])
            check([count(p) for p in devices] == [4, 4, 2], 'Native two-party histories mismatch')
            record('native certificate-validated WSS immediately synchronizes two-party text')

            process.terminate()
            process.wait(timeout=20)
            text_file.write_text('退出服务器期间排队，重启后重试', encoding='utf-8')
            run([client_exe, 'client-queue', devices[0], text_file])
            failed_send = subprocess.run([str(client_exe), 'client-send', str(devices[0])], capture_output=True, timeout=45)
            check(failed_send.returncode != 0, 'Offline send falsely succeeded')
            with sqlite3.connect(local_db(devices[0])) as local:
                check(local.execute("SELECT count(*) FROM pending WHERE state='queued'").fetchone()[0] == 1, 'Offline queue discarded')
            process = start()
            run([client_exe, 'client-send', devices[0]])
            run([client_exe, 'client-sync', devices[1]])
            check(count(devices[0]) == 5 and count(devices[1]) == 5, 'Offline restart catch-up failed')
            record('native offline queue and process/server restart catch-up')

            # Stage 3: real stable copies, multiple Unicode/same-name attachments and interrupted native download.
            sources = [run_root / 'source-1', run_root / 'source-2']
            for directory in sources:
                directory.mkdir()
            large_data = os.urandom(9 * 1024 * 1024 + 77)
            selected = [p / '同名-中文 🎈.bin' for p in sources]
            selected[0].write_bytes(large_data)
            selected[1].write_bytes(b'')
            text_file.write_text('多附件稳定副本', encoding='utf-8')
            run([client_exe, 'client-attach', devices[0], text_file, other_login['user_id'], *selected])
            with sqlite3.connect(local_db(devices[0])) as local:
                stable = local.execute('SELECT local_id,total,sha256 FROM pending_files ORDER BY rowid').fetchall()
            check(len(stable) == 2 and stable[0][1] == len(large_data), 'Stable copy metadata missing')
            files_a = local_db(devices[0]).parent / 'files'
            check(hashlib.sha256((files_a / f'{stable[0][0]}.source').read_bytes()).hexdigest() == hashlib.sha256(large_data).hexdigest(),
                'Stable copy is not byte-identical')
            selected[0].write_bytes(b'changed original after selection')
            selected[1].unlink()
            run([client_exe, 'client-send', devices[0]])
            with sqlite3.connect(local_db(devices[0])) as local:
                uploaded = local.execute('SELECT object_id,total,sha256 FROM pending_files ORDER BY rowid').fetchall()
            large_id = uploaded[0][0]
            files_b = local_db(devices[1]).parent / 'files'
            part = files_b / f'{large_id}.part'
            downloader = subprocess.Popen([str(client_exe), 'client-sync', str(devices[1])], stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
            interrupted = False
            try:
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline and downloader.poll() is None:
                    if part.exists() and 1024 * 1024 <= part.stat().st_size < len(large_data):
                        downloader.terminate()
                        downloader.wait(timeout=20)
                        interrupted = True
                        break
                    time.sleep(0.005)
                check(interrupted, 'Could not interrupt native download at a persisted nonfinal block')
            finally:
                if downloader.poll() is None:
                    downloader.terminate()
                    downloader.wait(timeout=20)
            check(count(devices[1]) == 6, 'Incomplete attachment blocked durable body/cursor transaction')
            prefix = (part.stat().st_size // (1024 * 1024)) * (1024 * 1024)
            check(prefix > 0 and prefix < len(large_data), 'Interrupted partial file not retained')
            resumed = run([client_exe, 'client-sync', devices[1]])
            received_lines = [line for line in resumed.stdout.decode('ascii').splitlines() if line.startswith('Downloaded bytes:')]
            check(received_lines and int(received_lines[-1].split(':')[1]) == len(large_data) - prefix,
                'Native download restarted instead of resuming persisted blocks')
            run([client_exe, 'client-sync', devices[2]])
            for directory in devices:
                with sqlite3.connect(local_db(directory)) as local:
                    saved = local.execute('SELECT object_id,total,sha256,state,saved_bytes FROM downloads').fetchall()
                check(len(saved) == 2 and all(row[3] == 'complete' and row[1] == row[4] for row in saved), 'Related device marked incomplete attachment saved')
                managed = local_db(directory).parent / 'files'
                for object_id, size, digest, _, _ in saved:
                    copied = (managed / f'{object_id}.file').read_bytes()
                    check(len(copied) == size and hashlib.sha256(copied).hexdigest() == digest, 'Native downloaded SHA-256 mismatch')
            check([count(p) for p in devices] == [6, 6, 3], 'Multiattachment histories mismatch')
            record('stable copy survives original change/deletion; Unicode same-name and empty files; all related devices save complete verified copies')
            record('native process termination retains body and partial file; resume transfers only remaining bytes')
            run([client_exe, 'client-sync', devices[0]])
            with sqlite3.connect(local_db(devices[0])) as local:
                check(local.execute('SELECT count(*) FROM object_states WHERE user_id=?', (other_login['user_id'],)).fetchone()[0] == 2,
                    'Recipient complete-file acknowledgements did not reach sender')
                check(local.execute('SELECT count(*) FROM downloads WHERE saved_ack=0').fetchone()[0] == 0,
                    'Complete native downloads were not acknowledged')
            record('verified complete-file receipts replicated to sender separately from body save')

            # Commit gating and upload recovery use a real persisted encrypted first chunk.
            upload_data = os.urandom(1024 * 1024 + 7)
            upload_input = {'request_id': uuid.uuid4().hex, 'filename': '../../CON:unsafe?',
                'total_size': str(len(upload_data)), 'sha256': hashlib.sha256(upload_data).hexdigest()}
            code, upload = request('/api/v1/uploads', 'POST', upload_input, token)
            check(code == 200, 'Upload initialization failed')
            upload_id = upload['object_id']
            chunk_url = f'/api/v1/uploads/{upload_id}/chunks/0'
            code, _ = binary_request(chunk_url, 'PUT', upload_data[:1024 * 1024], token)
            check(code == 200, 'First upload chunk failed')
            chunk_file = data_root / 'objects' / upload_id / '0000000000000000.chunk'
            committed_ciphertext = chunk_file.read_bytes()
            code, _ = binary_request(chunk_url, 'PUT', upload_data[:1024 * 1024], token)
            check(code == 200 and chunk_file.read_bytes() == committed_ciphertext, 'Duplicate chunk was not idempotent')
            code, _ = binary_request(chunk_url, 'PUT', b'x' * (1024 * 1024), token)
            check(code == 409, 'Changed duplicate chunk accepted')
            code, _ = request(f'/api/v1/uploads/{upload_id}/complete', 'POST', token=token)
            check(code == 409, 'Incomplete upload reported complete')
            code, _ = request('/api/v1/messages', 'POST', {'request_id': uuid.uuid4().hex, 'body': '', 'attachments': [upload_id]}, token)
            check(code == 409, 'Message referenced incomplete object')
            code, _ = binary_request(f'/api/v1/attachments/{upload_id}/chunks/0', credential=token)
            check(code == 401, 'Uncommitted object could be downloaded as a message attachment')
            process.terminate()
            process.wait(timeout=20)
            process = start()
            code, status_upload = request(f'/api/v1/uploads/{upload_id}', token=token)
            check(code == 200 and status_upload['received_bitmap'] == '01', 'Upload progress lost on crash restart')
            code, _ = binary_request(f'/api/v1/uploads/{upload_id}/chunks/1', 'PUT', upload_data[1024 * 1024:], token)
            check(code == 200, 'Resumed upload final chunk failed')
            code, complete = request(f'/api/v1/uploads/{upload_id}/complete', 'POST', token=token)
            check(code == 200 and complete['state'] == 'complete', 'Complete upload not authenticated/published')
            code, _ = request('/api/v1/messages', 'POST', {'request_id': uuid.uuid4().hex, 'body': '', 'attachments': [upload_id]}, token)
            check(code == 200, 'Attachment-only self message failed')
            run([client_exe, 'client-sync', devices[0]])
            run([client_exe, 'client-sync', devices[1]])
            check(count(devices[0]) == 7 and count(devices[1]) == 7, 'Attachment-only message body did not synchronize')
            managed = local_db(devices[1]).parent / 'files'
            check((managed / f'{upload_id}.file').read_bytes() == upload_data and not (managed.parent / 'CON').exists(),
                'Attachment-only file or internal path isolation failed')
            record('upload crash resume, idempotent chunks, incomplete-reference rejection and attachment-only message')

            time.sleep(1.05)
            code, third = login('test-third', 'test-only-third-password-123', uuid.uuid4().hex)
            check(code == 200, 'Third isolation account login failed')
            code, _ = binary_request(f'/api/v1/attachments/{large_id}/chunks/0', credential=third['token'])
            check(code == 401, 'Nonparticipant could read attachment')
            code, _ = request('/api/v1/messages', 'POST', {'request_id': uuid.uuid4().hex, 'body': 'unauthorized reference',
                'attachments': [upload_id]}, other_login['token'])
            check(code == 401, 'Other account could claim attachment ownership')
            record('attachment participants and upload ownership enforced; unsafe display filename never used as path')

            for disk_file in (chunk_file, data_root / 'objects' / upload_id / 'manifest.bin'):
                original = disk_file.read_bytes()
                modified = bytearray(original)
                modified[-1] ^= 1
                disk_file.write_bytes(modified)
                try:
                    code, _ = binary_request(f'/api/v1/attachments/{upload_id}/chunks/0', credential=token)
                    check(code == 500, 'Tampered encrypted attachment or manifest accepted')
                finally:
                    disk_file.write_bytes(original)
            original = chunk_file.read_bytes()
            chunk_file.write_bytes(original[:-1])
            try:
                code, _ = binary_request(f'/api/v1/attachments/{upload_id}/chunks/0', credential=token)
                check(code == 500, 'Truncated encrypted attachment accepted')
            finally:
                chunk_file.write_bytes(original)
            code, clear = binary_request(f'/api/v1/attachments/{upload_id}/chunks/0', credential=token)
            check(code == 200 and clear == upload_data[:1024 * 1024], 'Valid ciphertext restoration failed')
            record('actual disk chunk/manifest tamper and ciphertext truncation rejected; intact authenticated bytes restored')
            from gui import verify_gui
            gui_root = run_root / 'native-GUI'
            gui_root.mkdir()
            (gui_root / 'client.ini').write_text(f'[server]\nurl={url}\n[sync]\nheartbeat_seconds=1\nconnection_timeout_seconds=4\nreconcile_seconds=300\n', encoding='utf-8')
            def cycle_network(ignore_heartbeats=False):
                nonlocal process, drop_heartbeats
                process.terminate()
                process.wait(timeout=20)
                drop_heartbeats = ignore_heartbeats
                process = start()
            verify_gui(client_exe, gui_root, url, cert, run_root, record, request, token, local_db,
                cycle_network, run_root / 'server-output.log')
            # Independent server/client storage policies must preserve identical wire text.
            process.terminate()
            process.wait(timeout=20)
            server_config = run_root / 'server-compression.ini'
            server_config.write_text(f'[server]\nurl={url}\n[storage]\ntext_compression=zstd\nzstd_level=9\n', encoding='utf-8')
            process = start()
            for directory, algorithm, level in [(devices[1], 'none', 1), (devices[2], 'zstd', 19)]:
                (directory / 'client.ini').write_text(f'[server]\nurl={url}\n[storage]\ntext_compression={algorithm}\nzstd_level={level}\n', encoding='utf-8')
            text_file.write_text('不同压缩策略逐字互读 🎈。' * 1500, encoding='utf-8')
            run([client_exe, 'client-queue', devices[1], text_file, other_login['user_id']])
            with sqlite3.connect(local_db(devices[1])) as local:
                request_id = local.execute('SELECT request_id FROM pending ORDER BY rowid DESC LIMIT 1').fetchone()[0]
            run([client_exe, 'client-send', devices[1]])
            run([client_exe, 'client-sync', devices[2]])
            with sqlite3.connect(local_db(devices[1])) as local:
                check(local.execute('SELECT compressed FROM messages WHERE request_id=?', (request_id,)).fetchone()[0] == 0, 'Disabled compression still compressed new body')
            with sqlite3.connect(local_db(devices[2])) as local:
                saved = local.execute('SELECT compressed,length(body),original_length FROM messages WHERE request_id=?', (request_id,)).fetchone()
                check(saved[0] == 1 and saved[1] < saved[2], 'Different client level did not compress received body')
            code, body_page = request('/api/v1/sync', 'POST', {'generation': info['generation'], 'after': str(int(received['next'])), 'limit': 100}, other_login['token'])
            check(code == 200 and any(event['message']['request_id'] == request_id and event['message']['body'] == text_file.read_text(encoding='utf-8') for event in body_page['events']), 'Server compressed storage changed wire text')
            process.terminate()
            process.wait(timeout=20)
            stats = run([server_exe, 'storage-status', '--data', data_root]).stdout.decode('utf-8', errors='replace')
            check(any(line.startswith('Text zstd:') and int(line.split(':')[1]) > 0 for line in stats.splitlines()), 'Server did not store configured text compressed')
            process = start()
            record('server zstd9 and clients none/zstd19 store independently and return identical UTF-8 text')
            # Cache corruption is preserved, then repaired from authenticated server content.
            corrupt_file = files_b / f'{large_id}.file'
            damaged = bytearray(corrupt_file.read_bytes())
            damaged[0] ^= 1
            corrupt_file.write_bytes(damaged)
            run([client_exe, 'client-sync', devices[1]])
            check(hashlib.sha256(corrupt_file.read_bytes()).hexdigest() == uploaded[0][2] and list(files_b.glob(f'{large_id}.corrupt.*')), 'Corrupt local cache was not preserved and repaired')
            record('local complete-cache corruption detected, preserved and automatically redownloaded')
            cached_source = run_root / '暂停上传的稳定副本.bin'
            cached_bytes = os.urandom(128 * 1024 + 3)
            cached_source.write_bytes(cached_bytes)
            text_file.write_text('缓存上传编号失效后重新绑定', encoding='utf-8')
            run([client_exe, 'client-attach', devices[1], text_file, 'self', cached_source])
            missing_object = uuid.uuid4().hex
            with sqlite3.connect(local_db(devices[1])) as local:
                local_id = local.execute('SELECT local_id FROM pending_files ORDER BY rowid DESC LIMIT 1').fetchone()[0]
                local.execute('UPDATE pending_files SET object_id=? WHERE local_id=?', (missing_object, local_id))
            missing_cache = files_b / f'{missing_object}.file'
            (files_b / f'{local_id}.source').rename(missing_cache)
            run([client_exe, 'client-send', devices[1]])
            with sqlite3.connect(local_db(devices[1])) as local:
                rebound = local.execute('SELECT object_id FROM pending_files WHERE local_id=?', (local_id,)).fetchone()[0]
                check(rebound != missing_object and local.execute("SELECT count(*) FROM pending WHERE state='queued'").fetchone()[0] == 0, 'Unknown cached upload could not be rebound')
            check(missing_cache.read_bytes() == cached_bytes and (files_b / f'{rebound}.file').read_bytes() == cached_bytes, 'Rebinding lost the stable file copy')
            record('missing cached upload ID recreates authenticated upload from retained stable copy without discarding valid local file')
            space_body = os.urandom(384 * 1024).hex()
            code, _ = request('/api/v1/messages', 'POST', {'request_id': uuid.uuid4().hex, 'body': space_body}, token)
            check(code == 200, 'Space rollback fixture submission failed')
            run([client_exe, '--test-space-sync', devices[1]])
            run([client_exe, 'client-sync', devices[1]])
            record('actual SQLite-full during sync preserves whole message page and cursor, then catches up')
            with sqlite3.connect(local_db(devices[1])) as local:
                original_row = local.execute('SELECT message_id,body,compressed,original_length FROM messages ORDER BY rowid LIMIT 1').fetchone()
                original_count = local.execute('SELECT count(*) FROM messages').fetchone()[0]
                local.execute('UPDATE messages SET body=?,compressed=0,original_length=3 WHERE message_id=?', (b'bad', original_row[0]))
                local.execute("UPDATE meta SET value='0' WHERE key='cursor'")
            inconsistent = subprocess.run([str(client_exe), 'client-sync', str(devices[1])], capture_output=True, timeout=60)
            check(inconsistent.returncode != 0 and b'protocol_error' in inconsistent.stdout, 'Changed body with existing message identity was accepted')
            with sqlite3.connect(local_db(devices[1])) as local:
                check(local.execute("SELECT value FROM meta WHERE key='cursor'").fetchone()[0] == '0' and local.execute('SELECT count(*) FROM messages').fetchone()[0] == original_count,
                    'Failed message page advanced cursor or partially committed')
                local.execute('UPDATE messages SET body=?,compressed=?,original_length=? WHERE message_id=?', (original_row[1], original_row[2], original_row[3], original_row[0]))
                local.executescript("DROP TABLE message_orders; DROP TABLE device_states; DROP TABLE object_states; DROP TABLE account_reads; DROP TABLE pending_reads; ALTER TABLE downloads DROP COLUMN saved_ack; DELETE FROM meta WHERE key IN('state_cursor','saved_ack_cursor'); PRAGMA user_version=3;")
                local.execute('INSERT INTO ui_messages VALUES(?,0,1)', (original_row[0],))
            run([client_exe, 'client-sync', devices[1]])
            snapshots = list(local_db(devices[1]).parent.glob('client.db.migration-v3.*.db'))
            check(snapshots, 'Client upgrade did not retain a recovery snapshot')
            with sqlite3.connect(local_db(devices[1])) as local:
                check(local.execute('SELECT count(*) FROM messages').fetchone()[0] == original_count and local.execute('SELECT count(*) FROM message_orders').fetchone()[0] == original_count and
                    local.execute('SELECT unread,notified FROM ui_messages WHERE message_id=?', (original_row[0],)).fetchone() == (0,1), 'Client migration lost history, order metadata or notification flags')
            with sqlite3.connect(max(snapshots, key=lambda path: path.stat().st_mtime_ns)) as local:
                check(local.execute('PRAGMA user_version').fetchone()[0] == 3 and local.execute('SELECT count(*) FROM messages').fetchone()[0] == original_count, 'Client recovery snapshot is not a complete old schema')
            record('inconsistent message replay rolls back whole page; nonempty client3 migration preserves history, files and notification state')
            time.sleep(1.1)
            code, quota_login = login('test-quota', 'test-only-quota-password-123', uuid.uuid4().hex)
            check(code == 200, 'Quota fixture login failed')
            quota_token = quota_login['token']
            code, _ = request('/api/v1/messages', 'POST', {'request_id': uuid.uuid4().hex, 'body': 'x' * 4000}, quota_token)
            check(code == 507, 'Text and metadata quota was not enforced')
            code, _ = request('/api/v1/uploads', 'POST', {'request_id': uuid.uuid4().hex, 'filename': 'quota.bin', 'total_size': '4096', 'sha256': '0' * 64}, quota_token)
            check(code == 507, 'Upload quota failed to reserve metadata')
            code, _ = request('/api/v1/messages', 'POST', {'request_id': uuid.uuid4().hex, 'recipient_user_id': quota_login['user_id'], 'body': 'x' * 4000}, token)
            check(code == 507, 'Receiver quota was not enforced')
            code, quota_history = request('/api/v1/sync', 'POST', {'generation': info['generation'], 'after': '0'}, quota_token)
            check(code == 200 and quota_history['next'] == '0' and not quota_history['events'], 'Rejected quota submission advanced history')
            record('sender/receiver text and attachment metadata quotas reject atomically with space error')
        record('application-private CA validation, no system trust changes')
        process.terminate()  # Intentional server process crash, only this isolated child.
        process.wait(timeout=20)
        process = start()
        status, restarted_info = request('/api/v1/server-info')
        check(status == 200 and restarted_info['server_id'] == info['server_id'] and restarted_info['generation'] == info['generation'], 'Server identity changed on restart')
        status, _ = request('/api/v1/devices', token=token)
        check(status == 200, 'Committed credential lost on server crash')
        record('crash restart preserves identity and committed credentials')
        time.sleep(1.05)
        status, refreshed = login('test-user', 'test-only-password-123', device)
        check(status == 200, 'Same-device re-login failed')
        status, _ = request('/api/v1/devices', token=token)
        check(status == 401, 'Old device token remains active after re-login')
        token = refreshed['token']
        status, _ = request('/api/v1/logout', 'POST', token=token)
        check(status == 200, 'Logout failed')
        status, _ = request('/api/v1/devices', token=token)
        check(status == 401, 'Revoked token accepted')
        record('same-device token rotation and logout revocation')
        duplicate = subprocess.run([str(server_exe), 'init', '--data', str(data_root)], capture_output=True, timeout=20)
        check(duplicate.returncode != 0, 'Concurrent manager acquired server directory')
        record('exclusive server and local management lock')
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=20)
        log.close()
    expired_cert = data_root / 'expired-cert.pem'
    expired = subprocess.run([str(openssl), 'x509', '-in', str(cert), '-signkey', str(key),
        '-not_before', '20240101000000Z', '-not_after', '20240102000000Z',
        '-out', str(expired_cert)], env=environment, capture_output=True, timeout=60)
    check(expired.returncode == 0, 'Cannot generate expired test certificate')
    combined.write_bytes(expired_cert.read_bytes() + key.read_bytes())
    expired_context = ssl.create_default_context(cafile=str(expired_cert))
    log = (run_root / 'expired-server-output.log').open('wb')
    process = start()
    try:
        expired_native = subprocess.run([str(client_exe), '--test-login', f'https://localhost:{port}',
            str(run_root / 'native-expired'), 'test-user', str(expired_cert)], capture_output=True, timeout=45)
        check(expired_native.returncode != 0, 'Native private-CA path accepted expired certificate')
        record('native private-CA path rejects expired certificate')
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
                expired_context.wrap_socket(sock, server_hostname='localhost').close()
        except ssl.SSLCertVerificationError as error:
            check('expired' in error.verify_message.lower(), 'Certificate failed for a reason other than expiry')
            record('HTTPS expired certificate rejected with isolated trust')
        else:
            raise RuntimeError('Expired certificate accepted')
    finally:
        process.terminate()
        process.wait(timeout=20)
        log.close()
    database = data_root / 'server.db'
    check(database.read_bytes()[:16] != b'SQLite format 3\x00', 'Server database has plaintext SQLite header')
    try:
        db = sqlite3.connect(database)
        try:
            db.execute('SELECT * FROM accounts').fetchall()
        finally:
            db.close()
    except sqlite3.DatabaseError:
        record('stock SQLite cannot open encrypted server database')
    else:
        raise RuntimeError('Stock SQLite read server database')
    report['result'] = 'PASS'
    report_path = run_root / 'results.json'
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f'Verification record: {report_path}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        if REPORT_CONTEXT:
            path, report = REPORT_CONTEXT
            report.update(result='FAIL', failure=str(error))
            path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
            print(f'Failed verification record: {path}', flush=True)
        raise
