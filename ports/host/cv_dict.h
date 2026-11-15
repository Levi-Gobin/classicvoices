// Community pronunciation dictionaries (github.com/nicopn/classicvoices-dicts):
// one file per language code (en.dic, en-us.dic ...) for the Vocalizer voices
// and macintalk.dic for the MacinTalk ones, in NVDA's speech dictionary format:
//
//     pattern <TAB> replacement <TAB> case sensitive (0/1) <TAB> type
//
// type 0: the pattern anywhere, 1: a regular expression (Python's syntax,
// run with PCRE2; \1 or \g<1> in the replacement), 2: a whole word. Lines
// starting with # are comments. In a replacement, ~~...~~ is X-SAMPA: said as
// those sounds by any voice (a nasal vowel at the end closes as ~~~). Inside
// it everything is literal, backslashes included (r\ is English r).
//
// A voice uses its narrowest file first (en-us.dic, then en.dic); an entry
// of the wider file with the same pattern and type as one of the narrower is
// left out. The person's own entries (<dir>/Mine/<file>, same format) come
// before all of them and are used even with the community dictionaries off;
// <dir>/Mine/<file>.removed lists the fetched entries they took out (pattern
// TAB type, one per line).
#pragma once
#include "cv_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CvDict CvDict;

// The dictionaries for a voice ("ios6/en-US", "mt105/Alex"; language
// "en-US"), from dir; NULL when there are none or PCRE2 isn't available.
// Cached, and read again when a file changes: don't free.
const CvDict *cv_dict_for_voice(const char *dir, const char *voice_key, const char *language);
// The same, with the fetched (community) files only when community is set: the person's own entries alone
// otherwise (NULL when there are none).
const CvDict *cv_dict_for_voice_ex(const char *dir, const char *voice_key, const char *language, int community);
// A dictionary from text in the same format (trying out an entry before it's saved). Kept: don't free.
const CvDict *cv_dict_from_text(const char *dic_text);

// Engine text for a phonetic span: ipa (UTF-8) as the voice says it, malloc'd,
// or NULL when it can't. joined: right after a word, nothing between them.
typedef char *(*CvDictPhonemeFn)(void *ctx, const char *ipa, int joined);

// text for the engine (malloc'd): the dictionary applied (dict may be NULL),
// then rules (may be NULL), then each phonetic span as the voice's phonemes.
char *cv_dict_speech_text(const CvDict *dict, const CvRules *rules, const char *text, CvDictPhonemeFn phoneme,
                          void *ctx);

// The same, keeping track of where each output byte came from (SAPI's word
// positions): map_in gives a source position for each byte of text (NULL:
// the byte offsets), *map_out (malloc'd) one for each byte of the result;
// phonemes map to the word they stand for.
char *cv_dict_speech_text_mapped(const CvDict *dict, const CvRules *rules, const char *text, const int32_t *map_in,
                                 int32_t **map_out, CvDictPhonemeFn phoneme, void *ctx);


#ifdef __cplusplus
}
#endif
