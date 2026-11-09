# speech

A plugin for the SDL 3 builds of the Kirikiri Z engine (krkrz_dev) that reads text aloud with the
system's own voices, for players who can't or don't want to read: each line spoken into a WAV
file the game plays like a voice line (so auto mode waits for it and the voice volume applies),
or, in a browser, spoken by the browser itself.

Written for [KAGEX](https://github.com/krkren/kagex), whose engines have it built in, but it
works with any game on those engines.

| Platform | What speaks | Mode |
|---|---|---|
| Windows | SAPI 5 (the voices in Settings > Time & language > Speech) | file |
| macOS (10.15+), iOS (13+) | AVSpeechSynthesizer | file |
| Linux | eSpeak NG (`espeak-ng`, or `espeak`) when it's installed | file |
| Android | the system's TextToSpeech engine | file |
| the web | the browser's `speechSynthesis` | direct |

## For scripts

```
Plugins.link("speech.dll");

if (Speech.available) {
    Speech.speak("Hello there.", %[rate: 110, file: Storages.getTemporaryName() + ".wav"]);
    // ... then, from a Timer:
    if (Speech.state == "done") buffer.open(storageNameOf(file)), buffer.play();
}
```

| Member | |
|---|---|
| `available` | whether the system has a speech engine the plugin can use |
| `mode` | `"file"`: `speak()` makes a WAV file the game plays; `"direct"`: the system speaks aloud itself (the web, where the browser's volume applies, not the game's) |
| `voices()` | the voices, an Array of `%[id, name, language]` (`language` a BCP 47 tag where the system gives one); may be empty until the engine is ready (Android, the web) |
| `speak(text, options)` | starts a line; options `voice` (an id), `rate`, `pitch`, `volume` (%, 100 as the voice is) and, in file mode, `file` (the WAV to make, a native path); true when it started. One line at a time: a new one cancels the one before |
| `state` | `"idle"`, `"busy"`, `"done"` or `"failed"` |
| `cancel()` | stops the line |
| `lastError` | why the last line failed (also in the log) |

## Notes for each platform

- **Windows**: the line is spoken on a thread of its own (SAPI's `Speak` waits), into a 22,050 Hz
  mono WAV. Rate and pitch go to SAPI's -10..10 scales (rate 10 about three times as fast, pitch
  10 an octave up). Voices are listed from the speech voices category.
- **macOS and iOS**: `writeUtterance:toBufferCallback:` fills buffers on another thread; the first
  channel is kept, as 16-bit, and written as a WAV when the last (empty) buffer comes.
- **Linux**: the text goes to eSpeak NG through a file (`-f`, `-b 1` for UTF-8) and the WAV comes
  back with `-w`; voices from `espeak-ng --voices`. Without eSpeak NG, `available` is false. A
  Steam Linux Runtime game sees only what the runtime has.
- **Android**: `TextToSpeech` is made without a listener, so a line is tried again until the
  engine is ready (up to about 8 seconds of polls), and it's done when the engine no longer speaks
  and the file has stopped growing. On Android 11 and later the app's manifest must ask to see
  speech engines:
  `<queries><intent><action android:name="android.intent.action.TTS_SERVICE" /></intent></queries>`.
- **The web**: browsers speak only after the page has had a click or a key (the game's first
  click does), and headless browsers often have no voices.

## Building

As part of the engine: add this repository's `plugins` folder to krkrz_dev's
`TVP_PLUGIN_FOLDERS` (after krkrz_dev's own `src/plugins`, whose tp_stub and ncbind it uses) and
`speech` to `TVP_PLUGINS`, or to a platform kit's `pluginFolders` and `plugins`. Each backend's
file builds only on its own platform; on Apple's the Objective-C++ one needs AVFoundation.

On its own, with its tests:

```
cmake -B build -DKRKRZ_DEV_DIR=/path/to/krkrz_dev && cmake --build build && ctest --test-dir build
```

`tests/speech_test.cpp` checks the WAV writer and XML escaping and, on Linux, the eSpeak backend
against a stand-in `espeak-ng` on `PATH`: its voices, a line, a failure, a cancel.

## Licence

0BSD (`LICENSE`).
