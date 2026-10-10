"""Owned-process CPU and memory sampling; percentage is one logical CPU."""
import ctypes
from ctypes import wintypes as w
import time
k=ctypes.WinDLL('kernel32',use_last_error=True)
p=ctypes.WinDLL('psapi',use_last_error=True)
class Counters(ctypes.Structure):
    _fields_=[('cb',w.DWORD),('faults',w.DWORD)]+[(name,ctypes.c_size_t) for name in ('peak_working','working','peak_paged','paged','peak_nonpaged','nonpaged','pagefile','peak_pagefile','private')]
k.GetProcessTimes.argtypes=[w.HANDLE,*([ctypes.POINTER(w.FILETIME)]*4)]
p.GetProcessMemoryInfo.argtypes=[w.HANDLE,ctypes.POINTER(Counters),w.DWORD]
def cpu(process):
    values=[w.FILETIME() for _ in range(4)]
    if not k.GetProcessTimes(int(process._handle),*[ctypes.byref(value) for value in values]): raise ctypes.WinError(ctypes.get_last_error())
    return sum(((value.dwHighDateTime<<32)|value.dwLowDateTime)/10_000_000 for value in values[2:])
def sample(processes,seconds=30):
    before={name:cpu(process) for name,process in processes.items()}; start=time.monotonic(); peak={name:0 for name in processes}
    while time.monotonic()-start<seconds:
        for name,process in processes.items():
            if process.poll() is not None: raise RuntimeError('Owned process exited during idle sampling')
            counters=Counters(); counters.cb=ctypes.sizeof(counters)
            if not p.GetProcessMemoryInfo(int(process._handle),ctypes.byref(counters),counters.cb): raise ctypes.WinError(ctypes.get_last_error())
            peak[name]=max(peak[name],counters.working)
        time.sleep(.25)
    wall=time.monotonic()-start
    return {name:{'seconds':wall,'cpu_percent_one_core':(cpu(process)-before[name])/wall*100,'sampled_peak_working_bytes':peak[name]} for name,process in processes.items()}
