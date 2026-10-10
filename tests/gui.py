"""Drive only the isolated Drop window; do not send keys to the user's desktop."""
import ctypes
from ctypes import wintypes as w
import hashlib
import json
import pathlib
import sqlite3
import struct
import subprocess
import time
import winreg

u = ctypes.WinDLL('user32', use_last_error=True)
u.SetProcessDPIAware()
g = ctypes.WinDLL('gdi32', use_last_error=True)
k = ctypes.WinDLL('kernel32', use_last_error=True)
u.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]
u.FindWindowW.restype = w.HWND
u.GetDlgItem.argtypes = [w.HWND, ctypes.c_int]
u.GetDlgItem.restype = w.HWND
u.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT, ctypes.POINTER(ctypes.c_size_t)]
u.SendMessageTimeoutW.restype = w.LPARAM
u.IsWindowVisible.argtypes = [w.HWND]
u.IsWindowEnabled.argtypes = [w.HWND]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.GetWindowRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
u.GetWindow.argtypes = [w.HWND,w.UINT]
u.GetWindow.restype = w.HWND
u.GetClassNameW.argtypes = [w.HWND,w.LPWSTR,ctypes.c_int]
u.GetDlgCtrlID.argtypes = [w.HWND]
u.GetWindowThreadProcessId.argtypes = [w.HWND,ctypes.POINTER(w.DWORD)]
u.GetWindowThreadProcessId.restype = w.DWORD
u.IsChild.argtypes = [w.HWND,w.HWND]
class GuiThreadInfo(ctypes.Structure):
    _fields_=[('cbSize',w.DWORD),('flags',w.DWORD),('active',w.HWND),('focus',w.HWND),('capture',w.HWND),
        ('menu',w.HWND),('move',w.HWND),('caret',w.HWND),('rect',w.RECT)]
u.GetGUIThreadInfo.argtypes = [w.DWORD,ctypes.POINTER(GuiThreadInfo)]
ENUM = ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
u.GetDC.argtypes = [w.HWND]
u.GetDC.restype = w.HDC
u.ReleaseDC.argtypes = [w.HWND, w.HDC]
u.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
u.RedrawWindow.argtypes = [w.HWND, ctypes.c_void_p, w.HRGN, w.UINT]
g.CreateCompatibleDC.argtypes = [w.HDC]
g.CreateCompatibleDC.restype = w.HDC
g.CreateCompatibleBitmap.argtypes = [w.HDC, ctypes.c_int, ctypes.c_int]
g.CreateCompatibleBitmap.restype = w.HBITMAP
g.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
g.SelectObject.restype = w.HGDIOBJ
g.DeleteObject.argtypes = [w.HGDIOBJ]
g.DeleteDC.argtypes = [w.HDC]
g.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, ctypes.c_void_p, ctypes.c_void_p, w.UINT]
k.GlobalAlloc.argtypes = [w.UINT, ctypes.c_size_t]
k.GlobalAlloc.restype = w.HGLOBAL
k.GlobalLock.argtypes = [w.HGLOBAL]
k.GlobalLock.restype = ctypes.c_void_p
k.GlobalUnlock.argtypes = [w.HGLOBAL]


def wait_for(condition, label, seconds=60):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.1)
    raise RuntimeError(label)


class Gui:
    def __init__(self, executable, root):
        self.root = pathlib.Path(root).resolve()
        digest = hashlib.sha256(str(self.root).lower().encode('utf-16le')).hexdigest()[:16]
        self.class_name = 'DropWindow-' + digest
        self.run_name = 'Drop-' + digest
        self.process = subprocess.Popen([str(executable), '--ui-root', str(self.root), '--background'],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW)
        self.window = None
        wait_for(self.find, 'Isolated Drop window did not start')
        wait_for(lambda: self.enabled(105), 'Drop worker did not complete initial load')

    def find(self):
        self.window = u.FindWindowW(self.class_name, None)
        return self.window and self.process.poll() is None

    def child(self, identifier):
        return u.GetDlgItem(self.window, identifier)

    def send(self, window, message, wp=0, lp=0):
        output = ctypes.c_size_t()
        ctypes.set_last_error(0)
        if not u.SendMessageTimeoutW(window, message, wp, lp, 2, 2000, ctypes.byref(output)):
            raise RuntimeError(f'Drop UI message {message:#x} failed (Windows error {ctypes.get_last_error()})')
        return output.value

    def set(self, identifier, text):
        buffer = ctypes.create_unicode_buffer(text)
        self.send(self.child(identifier), 0x000C, 0, ctypes.addressof(buffer))

    def text(self, identifier):
        if self.send(self.child(identifier), 0x000E) == 0:
            return ''
        buffer = ctypes.create_unicode_buffer(4096)
        self.send(self.child(identifier), 0x000D, len(buffer), ctypes.addressof(buffer))
        return buffer.value

    def enabled(self, identifier):
        return bool(u.IsWindowEnabled(self.child(identifier)))

    def click(self, identifier):
        self.send(self.window, 0x0111, identifier, self.child(identifier))

    def idle(self):
        wait_for(lambda: self.enabled(105), 'Drop job failed to complete')

    def file_dialog(self, identifier, paths):
        # Use the real native dialog. A process-local HDROP handle cannot be
        # passed directly across processes as a substitute for Shell drag/drop.
        u.PostMessageW(self.window,0x0111,identifier,self.child(identifier))
        owned=[]
        def find_dialog():
            owned.clear()
            def visit(window,unused):
                name=ctypes.create_unicode_buffer(80)
                u.GetClassNameW(window,name,len(name))
                if u.GetWindow(window,4)==self.window and name.value=='#32770':
                    owned.append(window)
                return True
            callback=ENUM(visit)
            u.EnumWindows(callback,0)
            return bool(owned)
        wait_for(find_dialog,'Native Drop file dialog did not open',10)
        dialog=owned[0]
        editors=[]
        def visit(window,unused):
            name=ctypes.create_unicode_buffer(80)
            u.GetClassNameW(window,name,len(name))
            if name.value=='Edit' and u.GetDlgCtrlID(window)==1148:
                editors.append(window)
            return True
        callback=ENUM(visit)
        def ready():
            editors.clear()
            u.EnumChildWindows(dialog,callback,0)
            return len(editors)==1 and u.IsWindowVisible(editors[0]) and u.IsWindowEnabled(u.GetDlgItem(dialog,1))
        value=' '.join('"'+str(path)+'"' for path in paths) if len(paths)>1 else str(paths[0])
        if identifier==120:
            info=GuiThreadInfo(); info.cbSize=ctypes.sizeof(info)
            def focused_filename():
                name=ctypes.create_unicode_buffer(80)
                thread=u.GetWindowThreadProcessId(dialog,None)
                if not u.GetGUIThreadInfo(thread,ctypes.byref(info)) or not u.IsChild(dialog,info.focus):
                    return False
                u.GetClassNameW(info.focus,name,len(name))
                return name.value=='Edit' and u.GetDlgCtrlID(info.focus) in (1001,1148)
            wait_for(focused_filename,'The isolated save dialog filename edit did not receive initial focus',10)
            text=ctypes.create_unicode_buffer(value)
            self.send(info.focus,0x000C,0,ctypes.addressof(text))
        else:
            wait_for(ready,'Cannot identify the ready isolated dialog filename edit',10)
            time.sleep(0.25)  # Let the native dialog finish its initialization callbacks.
            text=ctypes.create_unicode_buffer(value)
            self.send(editors[0],0x000C,0,ctypes.addressof(text))
        u.PostMessageW(u.GetDlgItem(dialog,1),0x00F5,0,0)
        wait_for(lambda: not find_dialog(),'Native Drop file dialog did not close',15)

    def capture(self, destination):
        from PIL import Image
        self.send(self.window, 0x8003)
        if not u.RedrawWindow(self.window,None,None,0x1|0x4|0x80|0x100|0x400):
            raise RuntimeError('Cannot redraw owned Drop window before capture')
        time.sleep(.1)
        rect = w.RECT()
        u.GetWindowRect(self.window, ctypes.byref(rect))
        width, height = rect.right-rect.left, rect.bottom-rect.top
        dc = u.GetDC(self.window)
        memory = g.CreateCompatibleDC(dc)
        bitmap = g.CreateCompatibleBitmap(dc, width, height)
        old = g.SelectObject(memory, bitmap)
        try:
            if not u.PrintWindow(self.window, memory, 2):
                raise RuntimeError('Cannot capture actual Drop window')
            header = struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
            info = ctypes.create_string_buffer(header + bytes(1024))
            pixels = ctypes.create_string_buffer(width*height*4)
            g.SelectObject(memory, old)
            if g.GetDIBits(memory, bitmap, 0, height, pixels, info, 0) != height:
                raise RuntimeError('Cannot read actual Drop window pixels')
            rendered=Image.frombytes('RGB', (width, height), pixels.raw, 'raw', 'BGRX')
            if rendered.crop((20,60,width-20,height-20)).getbbox() is None:
                raise RuntimeError('Owned Drop window capture has an unpainted client area')
            rendered.save(destination)
        finally:
            g.SelectObject(memory, old)
            g.DeleteObject(bitmap)
            g.DeleteDC(memory)
            u.ReleaseDC(self.window, dc)

    def close(self):
        if self.process.poll() is None:
            def cancel(window,unused):
                if u.GetWindow(window,4)==self.window:
                    button=u.GetDlgItem(window,2)
                    if button:
                        u.PostMessageW(button,0x00F5,0,0)
                return True
            callback=ENUM(cancel)
            u.EnumWindows(callback,0)
            time.sleep(0.1)
            u.PostMessageW(self.window, 0x0111, 126, self.child(126))
            self.process.wait(timeout=10)
        if self.process.returncode != 0:
            raise RuntimeError(f'Drop did not exit successfully (exit {self.process.returncode})')


def verify_gui(executable, root, url, ca, run_root, record, request, token, local_db, cycle_network=None, server_log=None):
    gui = Gui(executable, root)
    try:
        gui.set(100, url)
        gui.set(101, str(ca))
        gui.click(104)
        gui.idle()
        if '连接成功' not in gui.text(121):
            raise RuntimeError('Native UI connection test failed')
        gui.set(102, 'test-user')
        gui.set(103, 'test-only-password-123')
        gui.click(105)
        gui.idle()
        if not gui.enabled(109) or gui.text(103):
            raise RuntimeError('Native UI login or password clearing failed')
        with sqlite3.connect(local_db(root)) as db:
            if db.execute('SELECT count(*) FROM messages').fetchone()[0] == 0 or db.execute("SELECT count(*) FROM downloads WHERE state!='complete'").fetchone()[0] == 0:
                raise RuntimeError('Fresh native UI did not publish durable bodies while larger attachments remained pending')
        if '附件待完成' not in gui.text(121):
            raise RuntimeError('Native UI did not show attachment catch-up progress')
        record('fresh device publishes body history and progress before complete attachment catch-up')
        source = run_root / '界面附件 🧪.txt'
        source.write_text('托盘界面选择的稳定副本', encoding='utf-8')
        empty=run_root/'界面空附件.txt'
        empty.write_bytes(b'')
        gui.set(107, '界面正文 🎈')
        gui.file_dialog(108,[source,empty])
        if '（2）' not in gui.text(108):
            raise RuntimeError('Native UI multi-file selection did not accept Chinese filename')
        gui.click(109)
        gui.idle()
        if gui.text(107):
            raise RuntimeError('Native UI did not persist and submit draft')
        def own_files_complete():
            with sqlite3.connect(local_db(root)) as db:
                return db.execute("SELECT count(*) FROM downloads d JOIN messages m USING(message_id) WHERE m.body=? AND d.state='complete'", ('界面正文 🎈'.encode(),)).fetchone()[0] == 2
        wait_for(own_files_complete, 'Native UI selected attachment jobs did not complete', 20)
        with sqlite3.connect(local_db(root)) as db:
            row = db.execute('SELECT count(*) FROM messages').fetchone()[0]
            if row < 1 or db.execute("SELECT count(*) FROM downloads WHERE state='complete'").fetchone()[0] < 1:
                raise RuntimeError('Native UI attachment was not completely saved')
        record('native tray UI private-CA probe/login, masked password clearing, Chinese body and file send')
        gui.set(114,'界面正文')
        gui.click(124)
        gui.idle()
        if gui.send(gui.child(117),0x1004)!=1:
            raise RuntimeError('Native local body search did not return one matching message')
        gui.send(gui.child(117),0x0100,0x24,0)  # HOME selects first history record.
        wait_for(lambda: gui.text(118)=='界面正文 🎈','Native selected history body did not display')
        gui.idle()
        if gui.send(gui.child(119),0x0146)!=2:
            raise RuntimeError('Native history attachment list did not contain both files')
        if source.name not in gui.text(119):
            raise RuntimeError('Native Chinese/emoji attachment display changed the original filename')
        exported=run_root/'界面显式导出.txt'
        gui.file_dialog(120,[exported])
        wait_for(exported.exists,'Native explicit attachment export did not finish')
        gui.idle()
        if exported.read_bytes()!=source.read_bytes():
            raise RuntimeError('Native explicit attachment export changed bytes')
        record('native local body search, complete Chinese/emoji history, multiple attachment display and explicit export')
        gui.capture(run_root / 'tray-ui.png')
        gui.set(114,'')
        gui.click(124)
        gui.idle()
        gui.set(106,'test-other')
        gui.set(107,'界面发送给另一账号')
        gui.click(109)
        gui.idle()
        with sqlite3.connect(local_db(root)) as db:
            row=db.execute('SELECT count(*) FROM messages').fetchone()[0]
            if db.execute("SELECT count(*) FROM messages WHERE sender!=recipient AND body=?",('界面发送给另一账号'.encode(),)).fetchone()[0]!=1:
                raise RuntimeError('Native UI recipient selection did not commit two-party message')
        record('native UI default-self and explicit recipient account selection')
        gui.send(gui.child(127), 0x014E, 1, 0)
        gui.send(gui.window, 0x0111, 127 | (1 << 16), gui.child(127))
        gui.idle()
        gui.send(gui.child(128), 0x014E, 18, 0)
        gui.send(gui.window, 0x0111, 128 | (1 << 16), gui.child(128))
        gui.idle()
        gui.set(106, '')
        gui.set(107, '界面压缩正文 🎈。' * 300)
        gui.click(109)
        gui.idle()
        with sqlite3.connect(local_db(root)) as db:
            packed = db.execute('SELECT compressed,length(body),original_length FROM messages ORDER BY rowid DESC LIMIT 1').fetchone()
            if packed[0] != 1 or packed[1] >= packed[2]:
                raise RuntimeError('Native compression setting did not store new text compressed')
        settings = json.loads((root / 'ui.json').read_text(encoding='utf-8'))
        if settings.get('text_compression') != 'zstd' or settings.get('zstd_level') != 19:
            raise RuntimeError('Native compression level was not persisted')
        gui.send(gui.child(127), 0x014E, 0, 0)
        gui.send(gui.window, 0x0111, 127 | (1 << 16), gui.child(127))
        gui.idle()
        gui.send(gui.window, 0x0218, 0x0012, 0)  # Dispatch the actual power-resume handler without sleeping Windows.
        gui.idle()
        row += 1
        record('native storage compression off/zstd level setting and power-resume handler')
        def count():
            with sqlite3.connect(local_db(root)) as db:
                return db.execute('SELECT count(*) FROM messages').fetchone()[0]
        if cycle_network and server_log:
            def traces(kind):
                return sum(line.startswith(kind) for line in server_log.read_text(encoding='utf-8', errors='replace').splitlines())
            before_heartbeat = traces('ws_heartbeat ')
            wait_for(lambda: traces('ws_heartbeat ') >= before_heartbeat + 3, 'Native app heartbeat did not reach server', 8)
            before_connections = traces('ws_open ')
            cycle_network(True)
            wait_for(lambda: traces('ws_open ') >= before_connections + 2, 'Missing heartbeat reply did not trigger native reconnection', 15)
            cycle_network(False)
            marker = '真实断线重连后的正文'
            status, _ = request('/api/v1/messages', 'POST', {'request_id': hashlib.sha256((str(run_root)+'reconnect').encode()).hexdigest()[:32], 'body': marker}, token)
            if status != 200:
                raise RuntimeError('Cannot seed reconnection body fixture')
            wait_for(lambda: count() == row + 1, 'Native reconnect did not catch up persisted body', 15)
            row += 1
            record('native heartbeat replies, missing-reply timeout, backoff reconnect and actual server restart catch-up')
        for index in range(55):
            status,_=request('/api/v1/messages','POST',{'request_id':hashlib.sha256((str(run_root)+'gui-page-'+str(index)).encode()).hexdigest()[:32],
                'body':f'界面分页记录 {index:02d}'},token)
            if status!=200:
                raise RuntimeError('Cannot seed native history pagination fixture')
        wait_for(lambda:count()==row+55,'Native history pagination fixture did not synchronize')
        gui.set(114,'')
        gui.click(124)
        gui.idle()
        if gui.send(gui.child(117),0x1004)!=50 or not gui.enabled(116):
            raise RuntimeError('Native first history page did not show exactly 50 records')
        gui.click(116)
        gui.idle()
        if gui.send(gui.child(117),0x1004)!=row+5 or gui.enabled(116):
            raise RuntimeError('Native second history page omitted or repeated records')
        gui.set(122,'2099-01-01')
        gui.set(123,'2099-01-02')
        gui.click(124)
        gui.idle()
        if gui.send(gui.child(117),0x1004)!=0:
            raise RuntimeError('Native local date filter did not exclude older history')
        gui.set(122,'')
        gui.set(123,'')
        gui.click(124)
        gui.idle()
        row=count()
        record('native 50-row history pagination and local date range filter')
        duplicate = subprocess.run([str(executable), '--ui-root', str(root)], capture_output=True, timeout=10)
        if duplicate.returncode != 0 or gui.process.poll() is not None:
            raise RuntimeError('Duplicate native UI did not activate existing instance')
        gui.send(gui.window, 0x0010)  # WM_CLOSE hides; it must not stop the worker.
        if u.IsWindowVisible(gui.window) or gui.process.poll() is not None:
            raise RuntimeError('Close-window did not retain tray process')
        before = row
        status, _ = request('/api/v1/messages', 'POST', {'request_id': hashlib.sha256(b'gui-incoming').hexdigest()[:32],
            'body': '隐藏窗口后实时同步'}, token)
        if status != 200:
            raise RuntimeError('Cannot send tray realtime fixture')
        wait_for(lambda: count() == before+1, 'Hidden tray did not receive WSS message', 15)
        record('native close-window retains tray and immediately synchronizes; duplicate start reuses instance')
        # Exercise user-owned Run value, then remove only this isolated instance's value.
        gui.send(gui.child(113), 0x00F1, 1, 0)
        gui.click(113)
        gui.idle()
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Run') as key:
            value, _ = winreg.QueryValueEx(key, gui.run_name)
            if '--background' not in value or str(root).lower() not in value.lower():
                raise RuntimeError('Native per-user autostart command incorrect')
        gui.send(gui.child(113), 0x00F1, 0, 0)
        gui.click(113)
        gui.idle()
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Run') as key:
            try:
                winreg.QueryValueEx(key, gui.run_name)
            except FileNotFoundError:
                pass
            else:
                raise RuntimeError('Native autostart disable left isolated value')
        record('native autostart enable/disable updates only isolated per-user Run value')
        gui.close()
        status, _ = request('/api/v1/messages', 'POST', {'request_id': hashlib.sha256(b'gui-offline').hexdigest()[:32],
            'body': '真正退出后重启补齐'}, token)
        if status != 200 or count() != before+1:
            raise RuntimeError('Exited UI remained synchronizing')
        gui = Gui(executable, root)
        wait_for(lambda: count() == before+2, 'Restarted UI did not catch up')
        record('native true exit stops synchronization; restart catches up durable history')
    finally:
        if gui.process.poll() is None:
            gui.close()
        # Failure cleanup never modifies another Drop autostart value.
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Microsoft\Windows\CurrentVersion\Run', 0, winreg.KEY_SET_VALUE) as key:
            try:
                winreg.DeleteValue(key, gui.run_name)
            except FileNotFoundError:
                pass
