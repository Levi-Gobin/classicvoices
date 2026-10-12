# ctypes binding for cvengine32/64.dll (the Vocalizer engine emulator).
import ctypes
import os
import struct

_lib = None


def load():
    global _lib
    if _lib is not None:
        return _lib
    bits = 64 if struct.calcsize("P") == 8 else 32
    path = os.path.join(os.path.dirname(__file__), "cvengine%d.dll" % bits)
    lib = ctypes.CDLL(path)
    c = ctypes
    lib.cv_abi_version.restype = c.c_int
    lib.vxs_create.restype = c.c_void_p
    lib.vxs_create.argtypes = [c.c_char_p]
    lib.vxs_destroy.restype = None
    lib.vxs_destroy.argtypes = [c.c_void_p]
    lib.vxs_set_asset_root.restype = None
    lib.vxs_set_asset_root.argtypes = [c.c_void_p, c.c_char_p]
    lib.vxs_start.restype = None
    lib.vxs_start.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p,
                              c.c_int, c.c_int, c.c_int, c.c_int]
    lib.vxs_cancel.restype = None
    lib.vxs_cancel.argtypes = [c.c_void_p]
    lib.vxs_preload.restype = None
    lib.vxs_preload.argtypes = [c.c_void_p, c.c_char_p]
    lib.vxs_engine_pitch.restype = c.c_int
    lib.vxs_engine_pitch.argtypes = [c.c_int]
    lib.vxs_last_error.restype = c.c_char_p
    lib.vxs_last_error.argtypes = [c.c_void_p]
    lib.vxs_last_first_audio_ms.restype = c.c_double
    lib.vxs_last_first_audio_ms.argtypes = [c.c_void_p]
    lib.cv_read_s16.restype = c.c_size_t
    lib.cv_read_s16.argtypes = [c.c_void_p, c.POINTER(c.c_int16), c.c_size_t,
                                c.POINTER(c.c_int)]
    # Recompiled (fast) engine DLLs, built from the user's firmware by
    # nvda/build_native.py; without them the interpreter is used.
    lib.cv_set_native_dir.restype = None
    lib.cv_set_native_dir.argtypes = [c.c_char_p]
    lib.cv_native_status.restype = c.c_longlong
    lib.cv_native_status.argtypes = [c.c_char_p]
    if lib.cv_abi_version() != 1:
        raise OSError("cvengine ABI mismatch")
    _lib = lib
    return lib
