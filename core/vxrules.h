// A quick pre-check for pronunciation rule sets (Apple's user_rules.txt and
// the newer packs' rules, thousands of regular expressions per language):
// each rule's pattern is read for a piece of literal text that every match
// must contain, and for a given text only the rules whose piece appears in it
// are worth running. A short phrase then runs a handful of rules instead of
// all of them. Each platform still runs the matching rules with its own
// regular expressions; this only says which ones can be skipped.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The longest literal every match of `pattern` contains (UTF-8, malloc'd;
// ASCII letters lowercased when icase), or NULL when there isn't a usable one
// (alternation at the top level, nothing but classes ...).
char *vx_rule_literal(const char *pattern, int icase);

typedef struct VxRuleIndex VxRuleIndex;
VxRuleIndex *vx_rule_index_new(void);
void vx_rule_index_free(VxRuleIndex *x);
// Rules are numbered by the caller, from 0; add them in any order.
void vx_rule_index_add(VxRuleIndex *x, uint32_t rule, const char *pattern, int icase);
// Sets marks[r] = 1 for every rule r < nrules that could match utf8_text:
// those whose literal appears in it, and those without a literal. Marks are
// only ever set, so after a rule rewrites the text, marking the new text adds
// the rules it may have brought into play.
void vx_rule_index_mark(const VxRuleIndex *x, const char *utf8_text, uint8_t *marks, size_t nrules);

#ifdef __cplusplus
}
#endif
