/* Suffix array construction by induced sorting (SA-IS, Nong, Zhang and
 * Chan 2009), linear time. */
#ifndef SQ_SAIS_H
#define SQ_SAIS_H

#include <stddef.h>
#include <stdint.h>

/* sa[0..n) = start positions of the suffixes of s[0..n) in sorted order
 * (a shorter suffix sorts before any suffix it is a prefix of). */
int sais(const uint8_t *s, int32_t *sa, int32_t n);

#endif
