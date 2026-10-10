"""Enter a generated test password only into an owned, hidden native console."""
import ctypes
from ctypes import wintypes as w
import subprocess
import time

k=ctypes.WinDLL('kernel32',use_last_error=True)
k.AttachConsole.argtypes=[w.DWORD]
k.GetConsoleMode.argtypes=[w.HANDLE,ctypes.POINTER(w.DWORD)]
k.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,ctypes.c_void_p,w.DWORD,w.DWORD,w.HANDLE]
k.CreateFileW.restype=w.HANDLE
k.CloseHandle.argtypes=[w.HANDLE]

class KeyEvent(ctypes.Structure):
    _fields_=[('down',w.BOOL),('repeat',w.WORD),('virtual',w.WORD),('scan',w.WORD),('char',w.WCHAR),('control',w.DWORD)]
class EventUnion(ctypes.Union):
    _fields_=[('key',KeyEvent),('padding',ctypes.c_byte*16)]
class InputRecord(ctypes.Structure):
    _fields_=[('kind',w.WORD),('event',EventUnion)]
k.WriteConsoleInputW.argtypes=[w.HANDLE,ctypes.POINTER(InputRecord),w.DWORD,ctypes.POINTER(w.DWORD)]

def masked_command(command,password):
    startup=subprocess.STARTUPINFO(); startup.dwFlags=subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow=0
    process=subprocess.Popen(list(map(str,command)),creationflags=subprocess.CREATE_NEW_CONSOLE,startupinfo=startup)
    attached=False; console=None
    try:
        k.FreeConsole()
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            if process.poll() is not None: raise RuntimeError('Masked console command exited before password entry')
            if not attached:
                attached=bool(k.AttachConsole(process.pid))
                if attached: console=k.CreateFileW('CONIN$',0xC0000000,3,None,3,0,None)
            mode=w.DWORD()
            if console and k.GetConsoleMode(console,ctypes.byref(mode)) and not mode.value&4: break
            time.sleep(.05)
        else: raise RuntimeError('Owned console never disabled password echo')
        entries=(InputRecord*(len(password)+1))()
        for item,char in zip(entries,password+'\r'):
            item.kind=1; item.event.key.down=1; item.event.key.repeat=1; item.event.key.char=char
            if char=='\r': item.event.key.virtual=13
        written=w.DWORD()
        if not k.WriteConsoleInputW(console,entries,len(entries),ctypes.byref(written)) or written.value!=len(entries): raise RuntimeError('Owned masked console input failed')
        process.wait(timeout=60)
        if process.returncode: raise RuntimeError('Masked management command failed')
    finally:
        if console: k.CloseHandle(console)
        if attached: k.FreeConsole()
        if process.poll() is None: process.terminate(); process.wait(timeout=10)
