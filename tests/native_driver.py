"""Drives the native Aqua_Filament library exactly like the Aquarius DynamicLibrary host (ABI 1)."""
import ctypes, json, sys, os
class Lib:
    def __init__(self, path):
        self.lib = ctypes.CDLL(os.path.abspath(path))
        assert self.lib.aqua_library_abi() == 1
        self.out = ctypes.create_string_buffer(1024 * 1024)
    def call(self, cmds, symbol='fl_call'):
        data = json.dumps({'cmds': cmds}).encode()
        fn = getattr(self.lib, symbol)
        n = fn(data, len(data), self.out, len(self.out))
        if n < 0: raise RuntimeError(f'call failed {n}')
        r = json.loads(self.out.raw[:n].decode())
        if not r.get('ok'): raise RuntimeError(r)
        return r['results']
    def shutdown(self):
        n = self.lib.fl_shutdown(b'', 0, self.out, len(self.out))
