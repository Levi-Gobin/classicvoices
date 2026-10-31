# ctypes binding for cvengine32/64.dll (the Vocalizer engine emulator).
import ctypes
import os
import struct

_lib = None


class Mark(ctypes.Structure):
    # VxsMark: a bookmark (ESC\mrk=N\) and the output frame it is reached at
    _fields_ = [("frame", ctypes.c_uint32), ("id", ctypes.c_uint32)]


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
    lib.vxs_set_rules_pack.restype = None
    lib.vxs_set_rules_pack.argtypes = [c.c_void_p, c.c_char_p]
    lib.vxs_engine_pitch.restype = c.c_int
    lib.vxs_engine_pitch.argtypes = [c.c_int]
    lib.vxs_last_error.restype = c.c_char_p
    lib.vxs_last_error.argtypes = [c.c_void_p]
    lib.vxs_last_first_audio_ms.restype = c.c_double
    lib.vxs_last_first_audio_ms.argtypes = [c.c_void_p]
    lib.vx_phoneme_text.restype = c.c_void_p
    lib.vx_phoneme_text.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p, c.c_int]
    lib.vxs_read_marks.restype = c.c_size_t
    lib.vxs_read_marks.argtypes = [c.c_void_p, c.POINTER(Mark), c.c_size_t]
    lib.cv_read_s16.restype = c.c_size_t
    lib.cv_read_s16.argtypes = [c.c_void_p, c.POINTER(c.c_int16), c.c_size_t,
                                c.POINTER(c.c_int)]
    # Recompiled (fast) engine DLLs, built from the user's firmware by
    # nvda/build_native.py; without them the interpreter is used.
    lib.cv_set_native_dir.restype = None
    lib.cv_set_native_dir.argtypes = [c.c_char_p]
    lib.cv_native_status.restype = c.c_longlong
    lib.cv_native_status.argtypes = [c.c_char_p]
    lib.cv_rules_text.restype = c.c_void_p
    lib.cv_rules_text.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.c_char_p, c.c_int, c.c_char_p, c.c_char_p]
    lib.cv_free.restype = None
    lib.cv_free.argtypes = [c.c_void_p]
    if lib.cv_abi_version() != 2:
        raise OSError("cvengine ABI mismatch")
    _lib = lib
    return lib


def phoneme_text(voices_root, downloads_root, voice_key, language, ipa, joined):
    """Engine text that says the IPA in this voice (core/vxphonemes.c), or
    None when it can't."""
    lib = load()
    p = lib.vx_phoneme_text(voices_root.encode("utf-8"), downloads_root.encode("utf-8"), voice_key.encode("utf-8"),
                            (language or "").encode("utf-8"), ipa.encode("utf-8"), 1 if joined else 0)
    if not p:
        return None
    try:
        return ctypes.string_at(p).decode("utf-8", "replace")
    finally:
        lib.cv_free(p)


def rules_text(voices_root, downloads_root, voice_key, language, text, own=True, pack=None):
    """text with Apple's pronunciation rules for the voice applied: the ones
    its own release had (own), then a newer pronunciation pack's (pack: its
    folder, or None)."""
    lib = load()
    if not text or (not own and not pack):
        return text
    p = lib.cv_rules_text(voices_root.encode("utf-8"), downloads_root.encode("utf-8"), voice_key.encode("utf-8"),
                          (language or "").encode("utf-8"), 1 if own else 0,
                          pack.encode("utf-8") if pack else None, text.encode("utf-8"))
    if not p:
        return text
    try:
        return ctypes.string_at(p).decode("utf-8", "replace")
    finally:
        lib.cv_free(p)
