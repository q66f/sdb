#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utf/utf.h"

static char *argv0 = NULL;

#include <errno.h>
#include <stdarg.h>
static inline void sysfatal(char *fmt, ...) {
  if (argv0) {
    write(2, argv0, strlen(argv0));
    write(2, ": ", 2);
  }

  va_list ap;
  u8int l = strlen(fmt) - 2;
  va_start(ap, fmt);
  if (memcmp(fmt + l, "%r", 2) == 0) {
    char t[l + 1], *errstr;

    memcpy(t, fmt, l);
    *(t + l) = 0;
    vfprintf(stderr, t, ap);
    errstr = strerror(errno);
    write(2, errstr, strlen(errstr));
    write(2, "\n", 1);
    exit(1);
  }
  vfprintf(stderr, fmt, ap);
  exit(1);
}

#define USED(x) ((void)(x))
#define ARGBEGIN \
        for((argv0 ? 0 : (argv0=*argv)), argv++, argc--; argv[0] && argv[0][0]=='-' && argv[0][1]; argc--, argv++){ \
                const char *_args, *_argt; \
                Rune _argc; \
                _args = &argv[0][1]; \
                if(_args[0]=='-' && _args[1]==0){ \
                        argc--; \
                        argv++; \
                        break; \
                }\
                _argc = 0; \
                while(*_args && (_args += chartorune(&_argc, _args))) \
                        switch(_argc)
#define ARGEND USED(_argt); USED(_argc); USED(_args);}USED(argv); USED(argc);
#define ARGF() (_argt=_args, _args="", (*_argt? _argt: argv[1]? (argc--, *++argv): 0))
#define ARGC() _argc
#define EARGF(x) (_argt=_args, _args="", (*_argt? _argt: argv[1]? (argc--, *++argv): (x, (char*)0)))
