"""Embed the owner's official icon only in an external installation executable.

Source control and build binaries retain the redistributable fallback icon.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import ctypes
from ctypes import wintypes
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SIZES = (16,32,48,64,128,256)


def embed(executable, source):
    executable,source = executable.resolve(),source.resolve()
    if executable.is_relative_to(ROOT) or source.is_relative_to(ROOT):
        raise ValueError('Official artwork must remain outside the repository')
    kernel = ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR,wintypes.BOOL]
    kernel.BeginUpdateResourceW.restype = wintypes.HANDLE
    kernel.UpdateResourceW.argtypes = [wintypes.HANDLE,ctypes.c_void_p,ctypes.c_void_p,wintypes.WORD,ctypes.c_void_p,wintypes.DWORD]
    kernel.UpdateResourceW.restype = wintypes.BOOL
    kernel.EndUpdateResourceW.argtypes = [wintypes.HANDLE,wintypes.BOOL]
    kernel.EndUpdateResourceW.restype = wintypes.BOOL
    with tempfile.TemporaryDirectory(prefix='godzilla-official-icon-') as folder:
        subprocess.run(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'tools/render_official_icon.ps1'),
                        '-Source',str(source),'-Out',folder],check=True,capture_output=True,creationflags=subprocess.CREATE_NO_WINDOW,timeout=30)
        frames = [(size,(Path(folder)/(str(size)+'.png')).read_bytes()) for size in SIZES]
    group = bytearray(struct.pack('<HHH',0,1,len(frames)))
    handle = kernel.BeginUpdateResourceW(str(executable),False)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        for index,(size,png) in enumerate(frames,101):
            data = ctypes.create_string_buffer(png)
            if not kernel.UpdateResourceW(handle,3,index,0,data,len(png)):
                raise ctypes.WinError(ctypes.get_last_error())
            group += struct.pack('<BBBBHHIH',size%256,size%256,0,0,1,32,len(png),index)
        data = ctypes.create_string_buffer(bytes(group))
        # 32512 is the compiler's fallback group; overwrite it so Explorer never picks the old icon.
        for group_id in (1,32512):
            if not kernel.UpdateResourceW(handle,14,group_id,0,data,len(group)):
                raise ctypes.WinError(ctypes.get_last_error())
        if not kernel.EndUpdateResourceW(handle,False):
            raise ctypes.WinError(ctypes.get_last_error())
        handle = None
    finally:
        if handle:
            kernel.EndUpdateResourceW(handle,True)
    # Reload as data (never execute code) and verify every embedded frame.
    kernel.LoadLibraryExW.argtypes = [wintypes.LPCWSTR,wintypes.HANDLE,wintypes.DWORD]
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceW.argtypes = [wintypes.HMODULE,ctypes.c_void_p,ctypes.c_void_p]
    kernel.FindResourceW.restype = wintypes.HANDLE
    kernel.LoadResource.argtypes = [wintypes.HMODULE,wintypes.HANDLE]
    kernel.LoadResource.restype = wintypes.HANDLE
    kernel.LockResource.argtypes = [wintypes.HANDLE]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.SizeofResource.argtypes = [wintypes.HMODULE,wintypes.HANDLE]
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.FreeLibrary.argtypes = [wintypes.HMODULE]
    module = kernel.LoadLibraryExW(str(executable),None,2)
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        for kind,index,expected in [(14,1,bytes(group))]+[(3,index,png) for index,(_,png) in enumerate(frames,101)]:
            resource = kernel.FindResourceW(module,index,kind)
            loaded = kernel.LoadResource(module,resource) if resource else None
            pointer = kernel.LockResource(loaded) if loaded else None
            if not pointer or kernel.SizeofResource(module,resource)!=len(expected) or ctypes.string_at(pointer,len(expected))!=expected:
                raise ValueError('Embedded official icon verification failed')
    finally:
        kernel.FreeLibrary(module)
