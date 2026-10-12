# Classic Voices synth driver for NVDA: the iOS 3-6 Vocalizer voices and the
# Mac OS X Lion / Mountain Lion / Mavericks voices, on Apple's original
# engines run in the cvengine emulator DLL.
import math

import addonHandler
import config
import nvwave
from speech.commands import BreakCommand, IndexCommand, PitchCommand
from synthDriverHandler import SynthDriver, VoiceInfo, synthDoneSpeaking, synthIndexReached

from ._classicvoices import engine as cv_engine
from ._classicvoices import lib as cv_lib
from ._classicvoices import paths as cv_paths
from ._classicvoices import voices as cv_voices

try:
    addonHandler.initTranslation()
except Exception:
    pass

OUT_RATE = 22050

# NVDA's 0-100 sliders mapped exponentially onto the engine's percent ranges,
# with the slider midpoint on a sensible default.
RATE_MIN, RATE_MAX = 50, 400      # percent of normal speaking rate
PITCH_MIN, PITCH_MAX = 50, 200    # percent of normal F0


def _exp_map(setting, lo, hi):
    return int(round(lo * (hi / lo) ** (setting / 100.0)))


def _exp_unmap(value, lo, hi):
    value = max(lo, min(hi, value))
    return int(round(100.0 * math.log(value / lo) / math.log(hi / lo)))


class SynthDriver(SynthDriver):
    name = "classicvoices"
    # Translators: description of the Classic Voices synthesizer.
    description = _("Classic Voices (iOS and Mac OS X Vocalizer)")

    supportedSettings = (
        SynthDriver.VoiceSetting(),
        SynthDriver.RateSetting(),
        SynthDriver.PitchSetting(),
        SynthDriver.VolumeSetting(),
    )
    supportedCommands = {IndexCommand, BreakCommand, PitchCommand}
    supportedNotifications = {synthIndexReached, synthDoneSpeaking}

    @classmethod
    def check(cls):
        try:
            cv_lib.load()
        except OSError:
            return False
        return bool(cv_voices.scan(cv_paths.voices_root(), cv_paths.downloads_root()))

    def __init__(self):
        super().__init__()
        cv_paths.ensure_dirs()
        self._player = self._make_player()
        self._voices = {}
        for v in cv_voices.scan(cv_paths.voices_root(), cv_paths.downloads_root()):
            self._voices[v.key] = v
        if not self._voices:
            raise RuntimeError("no Classic Voices data installed")
        self._rate = 50
        self._pitch = 50
        self._volume = 80
        self._voice = next(iter(self._voices))
        self._speaker = cv_engine.Speaker(
            cv_paths.voices_root(), cv_paths.downloads_root(),
            on_index=self._on_index, on_done=self._on_done,
            feed=self._feed, idle=self._idle,
            native_root=cv_paths.native_root())
        self._speaker.preload(self._voice)

    def _make_player(self):
        device = None
        for section, key in (("audio", "outputDevice"), ("speech", "outputDevice")):
            try:
                device = config.conf[section][key]
                break
            except KeyError:
                continue
        try:
            return nvwave.WavePlayer(channels=1, samplesPerSec=OUT_RATE,
                                     bitsPerSample=16, outputDevice=device)
        except TypeError:
            return nvwave.WavePlayer(channels=1, samplesPerSec=OUT_RATE, bitsPerSample=16)

    def terminate(self):
        try:
            self.cancel()
            self._speaker.close()
        finally:
            self._player.close()
            super().terminate()

    # ---- worker callbacks (worker thread)
    def _feed(self, data, onDone=None):
        self._player.feed(data, onDone=onDone)

    def _idle(self):
        self._player.idle()

    def _on_index(self, index):
        synthIndexReached.notify(synth=self, index=index)

    def _on_done(self):
        synthDoneSpeaking.notify(synth=self)

    # ---- speaking
    def speak(self, speechSequence):
        segments = []  # [(text, index)]
        text = []
        for item in speechSequence:
            if isinstance(item, str):
                text.append(item)
            elif isinstance(item, IndexCommand):
                segments.append((" ".join(text).strip(), item.index))
                text = []
            elif isinstance(item, BreakCommand):
                ms = max(1, min(5000, item.time))
                text.append("\x1b\\pause=%d\\" % ms)
            elif isinstance(item, PitchCommand):
                offset = max(0, min(100, self._pitch + item.offset))
                text.append("\x1b\\pitch=%d\\"
                            % cv_lib.load().vxs_engine_pitch(_exp_map(offset, PITCH_MIN, PITCH_MAX)))
            # CharacterModeCommand: single characters already arrive as text.
        if text:
            segments.append((" ".join(text).strip(), None))
        utt = cv_engine.Utterance(
            self._voice, segments,
            rate=_exp_map(self._rate, RATE_MIN, RATE_MAX),
            pitch=_exp_map(self._pitch, PITCH_MIN, PITCH_MAX),
            volume=self._volume)
        self._speaker.speak(utt)

    def cancel(self):
        self._speaker.cancel()
        self._player.stop()

    def pause(self, switch):
        self._player.pause(switch)

    # ---- settings
    def _get_rate(self):
        return self._rate

    def _set_rate(self, value):
        self._rate = max(0, min(100, value))

    def _get_pitch(self):
        return self._pitch

    def _set_pitch(self, value):
        self._pitch = max(0, min(100, value))

    def _get_volume(self):
        return self._volume

    def _set_volume(self, value):
        self._volume = max(0, min(100, value))

    def _get_voice(self):
        return self._voice

    def _set_voice(self, value):
        if value not in self._voices:
            raise ValueError("no such voice: %s" % value)
        self._voice = value
        self._speaker.preload(value)

    def _getAvailableVoices(self):
        out = {}
        for key, v in self._voices.items():
            lang = v.language.replace("-", "_") if v.language else None
            out[key] = VoiceInfo(key, v.name, lang)
        return out
