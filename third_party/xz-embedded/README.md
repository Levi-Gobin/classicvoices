# XZ Embedded

The `.xz` decoder by Lasse Collin and contributors, from
https://github.com/tukaani-project/xz-embedded (commit ae63ae3, tag
v2024-12-30), 0BSD licensed (see COPYING). Only the decoder files are here,
unchanged. The Android app uses it to unpack the Zig C compiler it downloads
to build fast engine code on the phone (android/jni/cv_untar.c).
