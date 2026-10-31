# Classic Voices synth driver for NVDA: the iOS 3-6 Vocalizer voices and the
# Mac OS X Lion / Mountain Lion / Mavericks voices, on Apple's original
# engines run in the cvengine emulator DLL.
import math
import re
import threading

import addonHandler
import config
import nvwave
from logHandler import log
from speech.commands import (BreakCommand, CharacterModeCommand, IndexCommand, PhonemeCommand,
                             PitchCommand, RateCommand, VolumeCommand)
from autoSettingsUtils.driverSetting import BooleanDriverSetting
from synthDriverHandler import SynthDriver, VoiceInfo, synthDoneSpeaking, synthIndexReached

from ._classicvoices import engine as cv_engine
from ._classicvoices import lib as cv_lib
from ._classicvoices import paths as cv_paths
from ._classicvoices import pronunciation as cv_pron
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


ESC_SPELL, ESC_NORMAL = "\x1b\\tn=spell\\", "\x1b\\tn=normal\\"


def _only_commands(text):
    return not re.sub(r"\x1b\\[^\\]*\\", "", text).strip()


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
        # Translators: a setting: the regular expressions iOS ran over text before the engine saw it.
        BooleanDriverSetting("pronunciationRules", _("Apple's pronunciation &rules"), defaultVal=True),
        # Translators: a setting: Apple's later pronunciation dictionary and rules, added to each voice's own.
        BooleanDriverSetting("newerPronunciation", _("&Newer pronunciation rules"), defaultVal=False),
    )
    supportedCommands = {IndexCommand, BreakCommand, PitchCommand, RateCommand, VolumeCommand,
                         CharacterModeCommand, PhonemeCommand}
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
        self._rules = True
        self._newer = False
        self._voice = next(iter(self._voices))
        self._speaker = cv_engine.Speaker(
            cv_paths.voices_root(), cv_paths.downloads_root(),
            on_index=self._on_index, on_done=self._on_done,
            feed=self._feed, idle=self._idle,
            native_root=cv_paths.native_root())
        self._speaker.preload(self._voice, self._pack(self._voice))

    def _pack(self, key):
        """The newer pronunciation pack for a voice, when that setting is on."""
        v = self._voices.get(key)
        if not self._newer or not v or key.startswith("mt"):
            return None
        return cv_pron.pack_dir(cv_paths.data_root(), v.language)

    _downloading = False

    def _get_packs(self):
        """Fetch the packs from Apple in the background, the first time."""
        cls = type(self)
        if cls._downloading or cv_pron.have_packs(cv_paths.data_root()):
            return
        cls._downloading = True

        def run():
            import ui
            try:
                cv_pron.download_all(cv_paths.data_root(), lambda message, fraction: None)
                # Translators: announced when the newer pronunciation rules have been downloaded.
                ui.message(_("Classic Voices: newer pronunciation rules are ready."))
            except Exception as e:
                log.error("Classic Voices: downloading pronunciation rules failed", exc_info=True)
                # Translators: announced when downloading the newer pronunciation rules failed; %s is why.
                ui.message(_("Classic Voices couldn't download the newer pronunciation rules: %s") % e)
            finally:
                cls._downloading = False
        threading.Thread(target=run, name="ClassicVoicesRules", daemon=True).start()

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
    @staticmethod
    def _changed(item, slider, value, lo, hi):
        """A prosody command's new engine value: an offset moves the slider,
        a multiplier scales what is heard (half the rate, twice the pitch);
        neither is back to the setting. value(slider) -> engine value."""
        offset = getattr(item, "offset", 0) or 0
        multiplier = getattr(item, "multiplier", 1)
        if multiplier is None:
            multiplier = 1
        v = value(max(0, min(100, slider + offset)))
        if multiplier != 1:
            v = v * multiplier
        return int(round(max(lo, min(hi, v))))

    def _phoneme(self, voice, ipa, joined):
        """ESC\\toi=lhp\\ ... for a PhonemeCommand's IPA, or None when this
        voice can't say it (the iOS 4 engine reads L&H+ out as text, and iOS 3
        says nothing for a consonant on its own; joined to a word it's fine)."""
        if not voice or not ipa:
            return None
        return cv_lib.phoneme_text(cv_paths.voices_root(), cv_paths.downloads_root(), voice.key, voice.language,
                                   ipa, joined)

    @staticmethod
    def _carry(state, kind, item, command, text):
        text.append(command)
        if getattr(item, "isDefault", False):  # back to the setting: nothing to carry on
            state.pop(kind, None)
        else:
            state[kind] = command

    def speak(self, speechSequence):
        text = []
        indexes = []
        voice = self._voices.get(self._voice)
        lib = cv_lib.load()
        # What the commands set lasts to the end of the sequence. An index is
        # a bookmark in the text: the sequence is one utterance, phrased whole.
        state = {}  # "pitch" / "rate" / "vol" / "tn" -> its ESC command
        prev = None
        pack = self._pack(self._voice)
        for item in speechSequence:
            if isinstance(item, str):
                if (self._rules or pack) and voice and "tn" not in state:
                    item = cv_lib.rules_text(cv_paths.voices_root(), cv_paths.downloads_root(), voice.key, voice.language,
                                             item, own=self._rules, pack=pack)
                text.append(item)
            elif isinstance(item, IndexCommand):
                text.append("\x1b\\mrk=%d\\" % item.index)
                indexes.append(item.index)
            elif isinstance(item, BreakCommand):
                ms = max(1, min(5000, item.time))
                text.append("\x1b\\pause=%d\\" % ms)
            elif isinstance(item, PitchCommand):
                pct = self._changed(item, self._pitch, lambda v: _exp_map(v, PITCH_MIN, PITCH_MAX),
                                    PITCH_MIN, PITCH_MAX)
                self._carry(state, "pitch", item, "\x1b\\pitch=%d\\" % lib.vxs_engine_pitch(pct), text)
            elif isinstance(item, RateCommand):
                pct = self._changed(item, self._rate, lambda v: _exp_map(v, RATE_MIN, RATE_MAX),
                                    RATE_MIN, RATE_MAX)
                self._carry(state, "rate", item, "\x1b\\rate=%d\\" % pct, text)
            elif isinstance(item, VolumeCommand):
                vol = self._changed(item, self._volume, lambda v: v, 0, 100)
                self._carry(state, "vol", item, "\x1b\\vol=%d\\" % vol, text)
            elif isinstance(item, CharacterModeCommand):
                if item.state:
                    state["tn"] = ESC_SPELL
                    text.append(ESC_SPELL)
                elif state.pop("tn", None):
                    text.append(ESC_NORMAL)
            elif isinstance(item, PhonemeCommand):
                # Sounds right after a word (nothing between them) are part of
                # it. MathCAT spells a letter and then gives a suffix ("x",
                # "-th"): a letter alone is said as its name anyway, so it's
                # said plainly and the suffix joins it ("exth", not "ex th").
                joined = isinstance(prev, str) and prev[-1:].strip() != ""
                letter = None
                if (not joined and isinstance(prev, CharacterModeCommand) and not prev.state
                        and len(text) >= 3 and text[-1] == ESC_NORMAL and text[-3] == ESC_SPELL
                        and len(text[-2].strip()) == 1 and text[-2].strip().lower() not in "ai"):
                    letter = text[-2].strip()
                lhp = self._phoneme(voice, item.ipa, joined or letter is not None)
                if lhp and letter is not None:
                    del text[-3:]
                    text.append(letter + lhp)
                elif lhp and joined and text:
                    text[-1] += lhp
                else:
                    text.append(lhp or item.text or "")
            prev = item
        text = " ".join(text).strip()
        utt = cv_engine.Utterance(
            self._voice, None if _only_commands(text) else text, indexes,
            rate=_exp_map(self._rate, RATE_MIN, RATE_MAX),
            pitch=_exp_map(self._pitch, PITCH_MIN, PITCH_MAX),
            volume=self._volume, pack=pack)
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

    def _get_pronunciationRules(self):
        return self._rules

    def _set_pronunciationRules(self, value):
        self._rules = bool(value)

    def _get_newerPronunciation(self):
        return self._newer

    def _set_newerPronunciation(self, value):
        self._newer = bool(value)
        if self._newer:
            self._get_packs()

    def _get_voice(self):
        return self._voice

    def _set_voice(self, value):
        if value not in self._voices:
            raise ValueError("no such voice: %s" % value)
        self._voice = value
        self._speaker.preload(value, self._pack(value))

    def _getAvailableVoices(self):
        out = {}
        for key, v in self._voices.items():
            lang = v.language.replace("-", "_") if v.language else None
            out[key] = VoiceInfo(key, v.name, lang)
        return out
