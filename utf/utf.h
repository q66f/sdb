#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum {
	Runeself = 0x80,
	Runeerror = 0xfffd,
	Runemax = 0x10ffff,
	UTFmax = 4,
};

#include <stdint.h>
typedef uint32_t Rune;

int chartorune(Rune *rune, const char *str);
int runetochar(char *str, const Rune *rune);
int runenlen(const Rune *r, int nrune);
int fullrune(const char *str, int n);
int runelen(Rune c);
Rune tolowerrune(Rune c);
Rune toupperrune(Rune c);
Rune totitlerune(Rune c);
int islowerrune(Rune c);
int isupperrune(Rune c);
int isalpharune(Rune c);
int istitlerune(Rune c);
int isspacerune(Rune c);
int isdigitrune(Rune c);

int utfnlen(const char *s, long m);

#ifdef __cplusplus
}
#endif
