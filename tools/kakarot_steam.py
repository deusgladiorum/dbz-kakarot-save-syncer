#!/usr/bin/env python3
"""Read/write DBZ Kakarot save files through Steam Remote Storage.

The game checks whether a slot exists with ISteamRemoteStorage::FileExists, which
consults Steam's per-app index (userdata/<id>/851850/remotecache.vdf), not the
folder on disk. A file copied into the folder from outside is therefore invisible
to the game ("No save data found") even though the save list, which comes from
SystemSave, shows it. Writing through the API fixes that and also makes the file
sync via Steam Cloud.

This loads the game's own steam_api64.dll (either build works) and initialises it
for AppID 851850. Requirements: the Steam client must be running and logged in,
and the game itself must NOT be running. While this tool runs, Steam shows you as
in-game for a few seconds.

Usage:
  kakarot_steam.py list
  kakarot_steam.py quota
  kakarot_steam.py exists NAME
  kakarot_steam.py write NAME LOCAL_FILE
  kakarot_steam.py read  NAME --out LOCAL_FILE
  kakarot_steam.py delete NAME        (removes from cloud and local index)
"""
from __future__ import annotations

import argparse
import ctypes as C
import glob
import os
import re
import sys
from pathlib import Path

APP_ID = "851850"
RS_VERSION = b"STEAMREMOTESTORAGE_INTERFACE_VERSION014"
GAME_DIR = r"steamapps\common\DRAGON BALL Z KAKAROT"
DLL_GLOBS = [
    r"Engine\Binaries\ThirdParty\Steamworks\*\Win64\steam_api64.dll",
    r"dlc\Remaster\Engine\Binaries\ThirdParty\Steamworks\*\Win64\steam_api64.dll",
]


def steam_libraries() -> list[str]:
    """The Steam install folder plus every library listed in steamapps\\libraryfolders.vdf."""
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam") as key:
            steam = winreg.QueryValueEx(key, "InstallPath")[0]
    except OSError:
        return []
    libraries = [steam]
    try:
        text = Path(steam, "steamapps", "libraryfolders.vdf").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return libraries
    seen = {os.path.normcase(os.path.normpath(steam))}
    for m in re.finditer(r'"path"\s+"((?:[^"\\]|\\.)*)"', text):
        path = m.group(1).replace("\\\\", "\\")
        norm = os.path.normcase(os.path.normpath(path))
        if norm not in seen:
            seen.add(norm)
            libraries.append(path)
    return libraries


def find_dll() -> str | None:
    """First steam_api64.dll of either game build found in any Steam library."""
    for library in steam_libraries():
        for pattern in DLL_GLOBS:
            found = sorted(glob.glob(os.path.join(library, GAME_DIR, pattern)))
            if found:
                return found[0]
    return None


class Steam:
    def __init__(self, dll_path: str | None):
        path = dll_path or find_dll()
        if not path:
            raise SystemExit("steam_api64.dll not found in any Steam library; pass --dll PATH")
        os.environ.setdefault("SteamAppId", APP_ID)
        os.environ.setdefault("SteamGameId", APP_ID)
        self.dll = C.WinDLL(path)
        d = self.dll
        d.SteamAPI_IsSteamRunning.restype = C.c_bool
        if not d.SteamAPI_IsSteamRunning():
            raise SystemExit("Steam client is not running")
        d.SteamAPI_Init.restype = C.c_bool
        if not d.SteamAPI_Init():
            raise SystemExit("SteamAPI_Init failed (is Steam logged in? is the game running?)")
        d.SteamAPI_GetHSteamUser.restype = C.c_int32
        d.SteamAPI_GetHSteamPipe.restype = C.c_int32
        d.SteamClient.restype = C.c_void_p
        user, pipe, client = d.SteamAPI_GetHSteamUser(), d.SteamAPI_GetHSteamPipe(), d.SteamClient()
        # ISteamClient::GetISteamRemoteStorage: prefer the flat export, else vtable slot 17
        # (stable across ISteamClient017..020).
        if hasattr(d, "SteamAPI_ISteamClient_GetISteamRemoteStorage"):
            f = d.SteamAPI_ISteamClient_GetISteamRemoteStorage
            f.restype, f.argtypes = C.c_void_p, [C.c_void_p, C.c_int32, C.c_int32, C.c_char_p]
            self.rs = f(client, user, pipe, RS_VERSION)
        else:
            vtable = C.cast(client, C.POINTER(C.c_void_p))[0]
            fn = C.cast(C.cast(vtable, C.POINTER(C.c_void_p))[17],
                        C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.c_int32, C.c_int32, C.c_char_p))
            self.rs = fn(client, user, pipe, RS_VERSION)
        if not self.rs:
            raise SystemExit("could not get ISteamRemoteStorage")
        p = "SteamAPI_ISteamRemoteStorage_"
        self._exists = self._fn(p + "FileExists", C.c_bool, [C.c_void_p, C.c_char_p])
        self._write = self._fn(p + "FileWrite", C.c_bool, [C.c_void_p, C.c_char_p, C.c_void_p, C.c_int32])
        self._read = self._fn(p + "FileRead", C.c_int32, [C.c_void_p, C.c_char_p, C.c_void_p, C.c_int32])
        self._size = self._fn(p + "GetFileSize", C.c_int32, [C.c_void_p, C.c_char_p])
        self._delete = self._fn(p + "FileDelete", C.c_bool, [C.c_void_p, C.c_char_p])
        self._count = self._fn(p + "GetFileCount", C.c_int32, [C.c_void_p])
        self._name_size = self._fn(p + "GetFileNameAndSize", C.c_char_p, [C.c_void_p, C.c_int32, C.POINTER(C.c_int32)])
        self._persisted = self._fn(p + "FilePersisted", C.c_bool, [C.c_void_p, C.c_char_p])
        self._quota = self._fn(p + "GetQuota", C.c_bool, [C.c_void_p, C.POINTER(C.c_uint64), C.POINTER(C.c_uint64)])
        self._cloud_app = self._fn(p + "IsCloudEnabledForApp", C.c_bool, [C.c_void_p])
        self._cloud_acct = self._fn(p + "IsCloudEnabledForAccount", C.c_bool, [C.c_void_p])

    def _fn(self, name, restype, argtypes):
        f = getattr(self.dll, name)
        f.restype, f.argtypes = restype, argtypes
        return f

    def close(self):
        self.dll.SteamAPI_Shutdown()

    def list(self):
        out = []
        for i in range(self._count(self.rs)):
            sz = C.c_int32()
            name = self._name_size(self.rs, i, C.byref(sz))
            out.append((name.decode(), sz.value, self._persisted(self.rs, name)))
        return out

    def quota(self):
        total, avail = C.c_uint64(), C.c_uint64()
        ok = self._quota(self.rs, C.byref(total), C.byref(avail))
        return ok, total.value, avail.value, self._cloud_app(self.rs), self._cloud_acct(self.rs)

    def exists(self, name: str) -> bool:
        return self._exists(self.rs, name.encode())

    def write(self, name: str, data: bytes) -> bool:
        return self._write(self.rs, name.encode(), data, len(data))

    def read(self, name: str) -> bytes:
        n = self._size(self.rs, name.encode())
        buf = C.create_string_buffer(n)
        got = self._read(self.rs, name.encode(), buf, n)
        return buf.raw[:got]

    def delete(self, name: str) -> bool:
        return self._delete(self.rs, name.encode())


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dll", help="path to the game's steam_api64.dll")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("quota")
    sub.add_parser("exists").add_argument("name")
    w = sub.add_parser("write"); w.add_argument("name"); w.add_argument("file")
    r = sub.add_parser("read"); r.add_argument("name"); r.add_argument("--out", required=True)
    sub.add_parser("delete").add_argument("name")
    a = p.parse_args(argv)

    s = Steam(a.dll)
    try:
        if a.cmd == "quota":
            ok, total, avail, app_on, acct_on = s.quota()
            print(f"GetQuota ok={ok}: total={total/1e6:.1f} MB, available={avail/1e6:.1f} MB, used={(total-avail)/1e6:.1f} MB; cloud enabled for app={app_on}, account={acct_on}")
        elif a.cmd == "list":
            for name, size, persisted in s.list():
                print(f"{name:16} {size:>10}  {'persisted' if persisted else 'local-only'}")
        elif a.cmd == "exists":
            print(f"{a.name}: {'exists' if s.exists(a.name) else 'NOT in Steam index'}")
        elif a.cmd == "write":
            data = Path(a.file).read_bytes()
            ok = s.write(a.name, data)
            print(f"write {a.name} ({len(data)} bytes): {'OK' if ok else 'FAILED'}; exists now: {s.exists(a.name)}")
            return 0 if ok else 1
        elif a.cmd == "read":
            data = s.read(a.name)
            Path(a.out).write_bytes(data)
            print(f"read {a.name}: {len(data)} bytes -> {a.out}")
        elif a.cmd == "delete":
            ok = s.delete(a.name)
            print(f"delete {a.name}: {'OK' if ok else 'FAILED'}")
            return 0 if ok else 1
    finally:
        s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
