# Vendored from Speech Dispatcher

`module_main.c`, `module_process.c`, `module_readline.c` and `spd_module_main.h`
are Speech Dispatcher's output-module helpers (the source of its
`libspeechd_module`), copied unchanged from https://github.com/brailcom/speechd
at the commit in `UPSTREAM`. They are under the BSD 2-clause licence stated in
each file (Copyright Samuel Thibault), so the module builds without
Speech Dispatcher's development package and runs with any Speech Dispatcher
from 0.10 on (server-side audio).

`speechd_types.h` and `spd_audio.h` here are minimal stand-ins written for
Classic Voices: just the types those helpers use, with the same layouts.
