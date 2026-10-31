# The speaking thread: takes utterances (text with NVDA's index markers
# written in as engine bookmarks, ESC\mrk=N\), runs them through the engine
# stream as one utterance, so an index never breaks the sentence it is in,
# and feeds the PCM to a player, cut at each bookmark so NVDA hears of the
# index once the audio before it has been played.
import ctypes
import queue
import threading
import time

from . import lib

OUT_RATE = 22050
CHUNK_FRAMES = 2205  # 100 ms
HOLD_BYTES = 2 * 220  # the last 10 ms, kept back to carry the index callback


class Utterance:
    def __init__(self, voice_key, text, indexes, rate, pitch, volume, pack=None):
        self.voice_key = voice_key
        self.pack = pack          # newer pronunciation pack folder (vxs_set_rules_pack), or None
        self.text = text          # engine text, or None when there is nothing to say
        self.indexes = indexes    # the NVDA indexes written into it, in order
        self.rate = rate          # percent of normal, 50-400
        self.pitch = pitch        # percent of normal, 50-200
        self.volume = volume      # 0-100


class Speaker:
    """Owns the VxStream and the worker thread. Callbacks are invoked on the
    worker thread; the caller (the synth driver) forwards them to NVDA."""

    def __init__(self, voices_root, downloads_root, on_index, on_done, feed, idle,
                 native_root=None):
        self._lib = lib.load()
        if native_root:
            self._lib.cv_set_native_dir(native_root.encode("utf-8"))
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

    def preload(self, voice_key, pack=None):
        self._lib.vxs_set_rules_pack(self._s, pack.encode("utf-8") if pack else None)
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

    def _speak(self, gen, utt):
        """Synthesize the utterance, feeding PCM as it arrives and each index
        with the audio before it. The last 10 ms are held back so the indexes
        at the end can come with real audio (holding back a whole chunk
        delayed the first sound until the next one came). Returns the indexes
        reported, and the pending tail (possibly b"")."""
        buf = (ctypes.c_int16 * CHUNK_FRAMES)()
        marks = (lib.Mark * 64)()
        done = ctypes.c_int(0)
        self._lib.vxs_set_rules_pack(self._s, utt.pack.encode("utf-8") if utt.pack else None)
        self._lib.vxs_start(self._s, utt.voice_key.encode("utf-8"), utt.text.encode("utf-8"),
                            utt.rate, utt.pitch, utt.volume, 0)
        wanted = set(utt.indexes)
        reached = []
        waiting = []  # (frame, index): known, its audio not here yet
        pending = b""
        start = 0  # the frame pending begins at
        while self._current(gen):
            n = self._lib.cv_read_s16(self._s, buf, CHUNK_FRAMES, ctypes.byref(done))
            if n:
                pending += ctypes.string_at(buf, n * 2)
            # a bookmark is known before its audio is read
            k = self._lib.vxs_read_marks(self._s, marks, 64)
            waiting += [(marks[i].frame, marks[i].id) for i in range(k) if marks[i].id in wanted]
            end = start + len(pending) // 2
            while waiting and waiting[0][0] <= end:
                frame, index = waiting.pop(0)
                cut = max(0, frame - start) * 2
                reached.append(index)
                # one sample of silence carries an index that falls where the
                # audio already given to the player ends
                self._feed(pending[:cut] or b"\0\0", lambda i=index: self._on_index(i))
                pending = pending[cut:]
                start += cut // 2
            if len(pending) > HOLD_BYTES:
                self._feed(pending[:-HOLD_BYTES])
                start += (len(pending) - HOLD_BYTES) // 2
                pending = pending[-HOLD_BYTES:]
            if done.value:
                return reached, pending
            if not n:
                time.sleep(0.001)
        return reached, b""

    def _run(self):
        while self._alive:
            item = self._q.get()
            if item is None:
                break
            gen, utt = item
            if not self._current(gen):
                continue
            reached, pending = self._speak(gen, utt) if utt.text else ([], b"")
            if not self._current(gen):
                continue
            # Indexes the engine never got to (past the end of the audio, or
            # nothing to say) are reached once everything before them is played.
            rest = [i for i in utt.indexes if i not in reached]
            if rest:
                def fire(rest=rest):
                    for i in rest:
                        self._on_index(i)
                self._feed(pending or b"\0\0", fire)
            elif pending:
                self._feed(pending)
            self._idle()
            self._on_done()
