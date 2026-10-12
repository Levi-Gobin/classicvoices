# The speaking thread: takes utterances (lists of text segments with NVDA
# index markers), runs them through the engine stream, and feeds the PCM to a
# player. One segment per index marker, the way the original VoiceOver spoke:
# each segment is its own engine utterance with the engine's natural
# end-of-utterance silence kept (vxstream does the same on iOS).
import ctypes
import queue
import threading
import time

from . import lib

OUT_RATE = 22050
CHUNK_FRAMES = 2205  # 100 ms


class Utterance:
    def __init__(self, voice_key, segments, rate, pitch, volume):
        self.voice_key = voice_key
        self.segments = segments  # [(text or None, index or None)]
        self.rate = rate          # percent of normal, 50-400
        self.pitch = pitch        # percent of normal, 50-200
        self.volume = volume      # 0-100


class Speaker:
    """Owns the VxStream and the worker thread. Callbacks are invoked on the
    worker thread; the caller (the synth driver) forwards them to NVDA."""

    def __init__(self, voices_root, downloads_root, on_index, on_done, feed, idle):
        self._lib = lib.load()
        self._s = self._lib.vxs_create(voices_root.encode("utf-8"))
        if not self._s:
            raise OSError("vxs_create failed")
        self._lib.vxs_set_asset_root(self._s, downloads_root.encode("utf-8"))
        self._on_index = on_index
        self._on_done = on_done
        self._feed = feed  # feed(bytes, onDone=None); may block on the audio device
        self._idle = idle  # called when an utterance finished feeding
        self._q = queue.Queue()
        self._gen = 0  # bumped by cancel(); the worker drops stale work
        self._lock = threading.Lock()
        self._alive = True
        self._thread = threading.Thread(target=self._run, name="ClassicVoices", daemon=True)
        self._thread.start()

    def speak(self, utterance):
        with self._lock:
            gen = self._gen
        self._q.put((gen, utterance))

    def cancel(self):
        with self._lock:
            self._gen += 1
        self._lib.vxs_cancel(self._s)

    def preload(self, voice_key):
        self._lib.vxs_preload(self._s, voice_key.encode("utf-8"))

    def close(self):
        self._alive = False
        self.cancel()
        self._q.put(None)
        self._thread.join(timeout=5)
        self._lib.vxs_destroy(self._s)
        self._s = None

    def last_error(self):
        return self._lib.vxs_last_error(self._s).decode("utf-8", "replace")

    def _current(self, gen):
        with self._lock:
            return gen == self._gen

    def _speak_segment(self, gen, utt, text):
        """Synthesize one segment, feeding PCM as it arrives. The last chunk is
        held back so the caller can attach an index callback to it. Returns the
        pending final chunk (possibly b"")."""
        buf = (ctypes.c_int16 * CHUNK_FRAMES)()
        done = ctypes.c_int(0)
        self._lib.vxs_start(self._s, utt.voice_key.encode("utf-8"), text.encode("utf-8"),
                            utt.rate, utt.pitch, utt.volume, 0)
        pending = b""
        while self._current(gen):
            n = self._lib.cv_read_s16(self._s, buf, CHUNK_FRAMES, ctypes.byref(done))
            if n:
                if pending:
                    self._feed(pending)
                pending = ctypes.string_at(buf, n * 2)
            if done.value:
                return pending
            if not n:
                time.sleep(0.004)
        return b""

    def _run(self):
        while self._alive:
            item = self._q.get()
            if item is None:
                break
            gen, utt = item
            if not self._current(gen):
                continue
            for text, index in utt.segments:
                if not self._current(gen):
                    break
                pending = self._speak_segment(gen, utt, text) if text else b""
                if not self._current(gen):
                    break
                if index is not None:
                    # The index is reached when its audio has been heard.
                    if pending:
                        self._feed(pending, lambda i=index: self._on_index(i))
                    else:
                        self._on_index(index)
                elif pending:
                    self._feed(pending)
            if self._current(gen):
                self._idle()
                self._on_done()
