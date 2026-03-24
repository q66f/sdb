/* https://stardict-4.sourceforge.net/StarDictFileFormat */
/* https://man.9front.org/2/rune */

#define _GNU_SOURCE
#include <ftw.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

#define nil NULL
#include "sdb.h"

#define islatin1(r) ((unsigned)(r) - 0xc0 < 0x40)
#define isupper(r) ((unsigned)(r) - 'A' < 26)

static const char *cflag;
static const char **dflag;
static s8int ndflag;
static bool aflag; /* suppress accent fold */
static bool fflag; /* suppress fuzzy ascii case */
static bool lflag; /* unicode case fold */

typedef struct Dict Dict;
typedef struct Offset Offset;
typedef struct Extent Extent;

struct Dict {
  u8int npath; /* length of base.off */
  u8int ntype; /* sametypesequence */
  u32int nidx; /* .idx word count */
  u32int nsyn; /* .syn word count */
};

struct Offset {
  u32int npage; /* total page count, equals to 2+(wordcount-1)/Pagesize, 2 for 0 and offset of last word */
  u32int *offset;  /* offset to page begining */
};

struct Extent {
  u32int size;
  u32int offset;
};

enum {
  /* sdcv ENTR_PER_PAGE = 32, after some calc, 64 is fine too */
  Pagesize = 64,

  /* .idx <=255 word| \0| 4 .dict offset| 4 .dict word size */
  /* .syn <=255 word| \0| 4 nth word in .idx */
  Entrysize = 264,
  Wordsize = 256,
  Idxseek = 9,
  Synseek = 5,
  Extentsize = 8, /* dynamic array? */
};

static size_t capacity;
static off_t *ifosize;
static char **path;
static Dict *dict;
static int *fdict;  /* fd */
static size_t ndict; /* matched dict number */

static Offset *offset;
static int *foffset; /* fd to .idx .syn */
static u8int noffset; /* essentially the number of matched idx (equal to ndict) and syn */

static Extent *extent;
static u8int nextent; /* matched extent number */

static char *pagebuf;
static u16int *pageoff;

static u8int latin1[] =
{
/*      Table to fold latin 1 characters to ASCII equivalents
                        based at Rune value 0xc0

         À    Á    Â    Ã    Ä    Å    Æ    Ç
         È    É    Ê    Ë    Ì    Í    Î    Ï
         Ð    Ñ    Ò    Ó    Ô    Õ    Ö    ×
         Ø    Ù    Ú    Û    Ü    Ý    Þ    ß
         à    á    â    ã    ä    å    æ    ç
         è    é    ê    ë    ì    í    î    ï
         ð    ñ    ò    ó    ô    õ    ö    ÷
         ø    ù    ú    û    ü    ý    þ    ÿ
*/
        'A', 'A', 'A', 'A', 'A', 'A', 'A', 'C',
        'E', 'E', 'E', 'E', 'I', 'I', 'I', 'I',
        'D', 'N', 'O', 'O', 'O', 'O', 'O',  0 ,
        'O', 'U', 'U', 'U', 'U', 'Y',  0 ,  0 ,
        'a', 'a', 'a', 'a', 'a', 'a', 'a', 'c',
        'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
        'd', 'n', 'o', 'o', 'o', 'o', 'o',  0 ,
        'o', 'u', 'u', 'u', 'u', 'y',  0 , 'y',
};

static void usage(void) {
  sysfatal("usage: sdb [-c dir] [-d dict] [-afl] [--] [pattern]\n"
           "    -c dir  Use dictionaries in given dir\n"
           "    -d dict Use the given dictionary\n"
           "            A list of available dictionaries is printed by -d "
           "(empty dict)\n"
           "    -a      Suppress latin1 accent fold of pattern\n"
           "    -f      Suppress ASCII case fuzzy match of pattern\n"
           "    -l      Do unicode case fold of pattern\n"
           "    --      Stop option parsing\n");
}

/* no symlink support */
static int filter(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
  if (ftwbuf->level > 2) {
    if (typeflag == FTW_D) return FTW_SKIP_SUBTREE;
    return 0;
  }

  if (typeflag != FTW_F) return 0;
  if (ndict >= capacity) {
    capacity = (capacity == 0) ? 16 : capacity * 2;
    path = realloc(path, capacity * sizeof(char *));
    ifosize = realloc(ifosize, capacity * sizeof(off_t *));
    if (!path) return -1;
  }
    
  if(memcmp(fpath + strlen(fpath) - 4, ".ifo", 4) == 0) {
    size_t l = strlen(fpath);
    path[ndict] = malloc(l + 5); /* .idx.off\0 or .dict.dz\0 minus .ifo */
    memcpy(path[ndict], fpath, l+1);
    ifosize[ndict++] = sb->st_size;
  }
  return 0;
}

static bool matchdflag(char *v, u16int nv) {
  u8int i;

  *(v + nv) = 0;
  for (i = 0; i < ndflag; i++)
    if (memcmp(*(dflag + i), v, nv + 1) == 0)
      return 0;
  return 1;
}

static void readifo(void) {
  size_t i = 0, nhole = 0;
  int fd;
  char *key, *tem, *val;
  u16int nval;
  bool freebuf = false;
  char *tempbuf;

  dict = malloc(ndict * sizeof(Dict));
  key = pagebuf;

  while(i < ndict) {
    if (nhole)
      *path = *(path + nhole);
    if ((fd = open(*path, O_RDONLY)) == -1)
      sysfatal("open %s: %r", *path);

    if (ifosize[i+nhole] > Entrysize * Pagesize) {
      if (!freebuf) {
        tempbuf = calloc(1, ifosize[i+nhole]);
        freebuf = true;
      } else {
        tempbuf = realloc(key, ifosize[i+nhole]);
      }
      key = tempbuf;
    }

    /* St.. + lf + version + lf = 39 */
    /* to deal with crlf */
    if (pread(fd, key, ifosize[i+nhole] - 38, 38) == -1 ||
        (tem = memchr(key, '\n', 3)) == nil)
      sysfatal("invalid %s\n", *path);

    dict->nsyn = 0;
    while (1) {
      key = tem + 1;
      /* 17 's longest and newline's not allowed per spec */
      if ((val = memchr(key, '=', 17 + 4)) == nil)
        break;
      *val++ = 0;
      if ((tem = strchr(val, '\n')) != nil) {
        if (*(tem - 1) != '\r')
          nval = tem - val;
        else
          nval = tem - val - 1;
      } else {
        nval = strlen(val);
        tem = val + nval; /* for break */
      }

      if (memcmp(key, "bookname", 8) == 0) {
        if (nval == 0)
          sysfatal("%s no bookname\n", *path);
        if (ndflag > 0 && matchdflag(val, nval) == 1) {
          ndict--;
          nhole++;
          free(*path);
          goto nextdict;
        } else if (ndflag == -1)
          write(1, val, nval);
      } else if (memcmp(key, "wordcount", 9) == 0) {
        dict->nidx = atol(val);
        if (dict->nidx < Pagesize)
          sysfatal("%s has less 64 word\n", *path);
        close(fd);
      } else if (memcmp(key, "synwordcount", 12) == 0) {
        dict->nsyn = atol(val);
        if (dict->nsyn < Pagesize)
          sysfatal("%s has less 64 word\n", *path);
      } else if (memcmp(key, "sametypesequence", 16) == 0) {
        if (isupper(*val) || nval > 1)
          sysfatal("%s sametypesequence type unsupported\n", *path);
        dict->ntype = 1;
      } else if (memcmp(key, "idxoffsetbits", 13) == 0)
        if (memcmp(val, "64", 2) == 0)
          sysfatal("idxoffsetbits=64 unsupported");
    }

    if (ndflag != -1) {
      dict->npath = strlen(*path);
      if (dict->nsyn)
        noffset++;
    } else
      printf("\t%u\t%u\n", dict->nidx, dict->nsyn);
    i++;
    path++;
    dict++;
  nextdict:
    close(fd);
  }
  if (ndflag == -1)
    exit(0);
  if (ndict == 0)
    sysfatal("no dict\n");
  noffset += ndict;
  if (freebuf) free(tempbuf);
  free(ifosize);
}

static int genoff(u32int wc, struct stat st, u8int entryseek) {
  int fd;
  u32int i, seek;
  u8int j;
  char *buf;

  buf = malloc(st.st_size);
  if (read(*foffset, buf, st.st_size) == -1) {
    *(*path + dict->npath) = 0; /* base.idx"\0"off */ 
    sysfatal("read %s: %r", *path);
  }

  *(offset->offset++) = 0;
  for (i = seek = 0; i < offset->npage - 2; i++) {
    for (j = 0; j < Pagesize; j++)
      seek += strlen(buf + seek) + entryseek;
    *(offset->offset++) = seek;
  }

  for (i = 0; i < wc % Pagesize - 1; i++)
    seek += strlen(buf + seek) + entryseek;
  *offset->offset = seek;
  offset->offset -= offset->npage - 1;

  if ((fd = open(*path, O_RDWR | O_CREAT | O_TRUNC, 0644)) == -1 ||
      write(fd, offset->offset, offset->npage * 4) == -1)
    sysfatal("gen %s: %r", *path);
  return fd;
}

/* https://github.com/projg2/portable-endianness */
static u32int readu32be(const u8int *p) {
  return ((u32int)p[0] << 24) | ((u32int)p[1] << 16) | ((u32int)p[2] << 8) | (u32int)p[3];
}

static void readoff(u32int wc, int entryseek) {
  int fd;
  struct stat s, st;

  offset->npage = 2 + (wc - 1) / Pagesize;
  offset->offset = malloc(offset->npage * 4);

  if ((*foffset = open(*path, O_RDONLY)) == -1) /* base.idx"\0"off */
    sysfatal("open %s: %r", *path);
  fstat(*foffset, &st);

  *(*path + dict->npath) = '.'; /* base.idx'.'off */

  if ((fd = open(*path, O_RDONLY)) == -1) {
    if (errno == ENOENT)
      fd = genoff(wc, st, entryseek);
    else
      sysfatal("open %s: %r", *path);
  }
restat:
  fstat(fd, &s);
  if (s.st_mtime < st.st_mtime) {
    fd = genoff(wc, st, entryseek);
    goto restat;
  }
  foffset++;

  if (pread(fd, offset->offset, s.st_size, 0) == -1)
    sysfatal("read off %s: %r", *path);
  close(fd);
  offset++;
}

/* do fuzzy matching instead of folding to the utf or latin1 is not possible because the way words are sorted */
static int cmp(char *w, char *p, u8int nw) {
  int i = strncasecmp(w, p, nw);
  if (i == 0 && fflag)
    return memcmp(w, p, nw);
  else
    return i;
}

static char *getword(u32int offset) {
  /* read Wordsize+1 is bad but i dont wanna change it until regex is added */
  if (pread(*foffset, pagebuf, Wordsize, offset) == -1)
    sysfatal("pread %s: %r", *path);
  return pagebuf;
}

/* .idx |.syn page no, Idxseek |Synseek */
static void loadpage(u32int npage, u8int entryseek) {
  u32int *tem, i;
  u8int j;

  tem = offset->offset + npage;
  i = *tem;
  pread(*foffset, pagebuf, *(tem + 1) - i, i);
  /* last page size < Pageszie, but extra bytes barely used */
  for (i = j = 0; j < Pagesize; j++, i += strlen(pagebuf + i) + entryseek)
    *(pageoff + j) = i;
}

static void idxextent(char *w, u8int nw) {
  *(w + nw - 1) = ' ';
  write(1, w, nw);

  extent->offset = readu32be((u8int *)w + nw);
  extent->size = readu32be((u8int *)w + nw + 4);
  nextent++;
  extent++;
}

static void synextent(char *w, u8int nw) {
  extent->size = readu32be((u8int *)w + nw);
  nextent++;
  extent++;
}

/* half way */
/* https://github.com/ianlewis/go-stardict/blob/master/dict/dict.go */
static void idxdict(void) {
  u8int i, typeseek;

  /* extent -= nextent for match idxextent()'s print order */
  for (i = 0, extent -= nextent; i < nextent; i++, extent++) {
    if (extent->size + 1 > Entrysize * Pagesize) /* newline */
      pagebuf = malloc(extent->size + 1);
    pread(*fdict, pagebuf, extent->size, extent->offset);

    if (dict->ntype == 1) {
      typeseek = 0;
    } else {
      if (isupper(*pagebuf))
        sysfatal("%s sametypesequence type unsupported\n", *path);
      typeseek = 1;
      extent->size--; /* null terminator */
    }
    /* adding html support is easy, just how to do it good enough */
    *(pagebuf + extent->size) = '\n';
    write(1, pagebuf + typeseek, extent->size - typeseek + 1);
  }
  extent -= nextent;
  nextent = 0;
}

static void syndict(void) {
  u8int i;
  u32int *tem, j;
  char *w;

  for (i = 0, extent -= nextent; i < nextent; i++, extent++) {
    tem = (offset - 1)->offset + (extent->size / Pagesize);
    j = *tem;
    /* foff - 1 is .idx, */
    pread(*(foffset - 1), pagebuf, *(tem + 1) - j, j);
    w = pagebuf;
    for (j = 0; j < extent->size % Pagesize; j++, w += strlen(w) + Idxseek)
      ;

    j = strlen(w) + 1;
    *(w + j - 1) = '\n';
    write(1, w, j);

    extent->offset = readu32be((u8int *)w + j);
    extent->size = readu32be((u8int *)w + j + 4);
  }
  idxdict();
}

/* boundless_binary_search */
/* https://github.com/scandum/binary_search/blob/master/binary_search.c */
/*
   search 1st word of each page,
   if the 1st word matched the the word we searching
     if doing fuzzy matching (these might be multiple extents) check at least 4 words back and forth.
     if not doing fuzzy matching, flush and return
   if its not matched, search the final page that the word may located at
   then if matched and fuzzy matching
   loop the whole page back and forth until failure or continue looping adjacent page
*/
/* entryseek is Idxseek or Synseek */
static void search(char *w, int entryseek, void (*extentsave)(char *, u8int),
            void (*dictflush)(void), u8int nw) {
  u32int bot, mid, boo, mii;
  int res;
  char *buf, *mnt;
  u16int nwseek = nw - 1 + entryseek;

  if (cmp(w, getword(0), nw) < 0 ||
      cmp(w, getword(*(offset->offset + offset->npage - 1)), nw) > 0)
    return;

  bot = 0;
  mid = offset->npage - 1; /* excluding the end word offset */
  while (mid > 1) {
    res = cmp(w, getword(*(offset->offset + bot + mid / 2)), nw);
    if (res > 0)
      bot += mid++ / 2;
    else if (res == 0) {
      bot += mid / 2;
      goto page1stmatched;
    }
    mid /= 2;
  }

  res = cmp(w, getword(*(offset->offset + bot)), nw);
  if (res < 0)
    bot--;
  else if (res == 0) {
  page1stmatched:
    extentsave(pagebuf, nw);
    if (!fflag) {
      if ((res = 4 * nwseek - Wordsize) > 0)
        pread(*foffset, pagebuf + Wordsize, res, *(offset->offset + bot) + Wordsize);

      for (buf = pagebuf + nwseek; nextent < 4; buf += nwseek)
        if (cmp(w, buf, nw) == 0)
          extentsave(buf, nw);
        else
          break;
      pread(*foffset, buf, 4 * nwseek, *(offset->offset + bot) - 4 * nwseek);
      for (; nextent < 8; buf += nwseek)
        if (cmp(w, buf, nw) == 0)
          extentsave(buf, nw);
        else
          break;
    }
    dictflush();
    return;
  }

  loadpage(bot, entryseek);

  /* not at the last page || page wordcount equals to Pagesize */
  mid = mii = (bot != offset->npage - 2 || dict->nidx % Pagesize == 0)
                  ? Pagesize
                  : dict->nidx % Pagesize;
  boo = 0;
  while (mii > 1) {
    res = cmp(w, pagebuf + *(pageoff + boo + mii / 2), nw);
    if (res > 0)
      boo += mii++ / 2;
    else if (res == 0) {
      boo += mii / 2;
      buf = mnt = pagebuf + *(pageoff + boo);
      extentsave(buf, nw);
      if (!fflag) {
        mii = boo = boo + 1;
      pageloop:
        for (; mii < mid + 1; mii++)
          if (cmp(w, mnt += nwseek, nw) == 0)
            extentsave(mnt, nw);
          else
            break;
        for (; boo > 1; boo--)
          if (cmp(w, buf -= nwseek, nw) == 0)
            extentsave(buf, nw);
          else
            break;
        if (mii == mid + 1 && bot + 1 < offset->npage) {
          mii = 1;
          loadpage(bot + 1, entryseek);
          mnt = pagebuf + *pageoff;
          goto pageloop;
        }
        if (boo == 0) {
          boo = Pagesize;
          loadpage(bot - 1, entryseek);
          buf = pagebuf + *(pageoff + Pagesize - 1);
          goto pageloop;
        }
      }
      if (nextent > Extentsize)
        nextent = Extentsize;
      dictflush();
      return;
    }
    mii /= 2;
  }
  if (cmp(w, buf = pagebuf + *(pageoff + boo), nw) == 0) {
    extentsave(buf, nw);
    dictflush();
  }
}

static void accentfold(char *cp) {
  Rune r;
  u8int i = 0; /* latin1 's 2 byte long while ascii 's 1 byte */

  while (*cp) {
    chartorune(&r, cp);
    if (islatin1(r) && latin1[r - 0xc0]) {
      *(cp - i++) = latin1[r - 0xc0];
      cp += 2;
    } else
      cp += runetochar(cp - i, &r);
  }
  *(cp - i) = 0;
}

static void utflowcase(char *cp) {
  Rune r;

  while (*cp) {
    chartorune(&r, cp);
    r = tolowerrune(r);
    cp += runetochar(cp, &r);
  }
}

static void sdb(char *w) {
  u8int i, nw;

  if (!aflag)
    accentfold(w);
  if (lflag)
    utflowcase(w);
  nw = strlen(w) + 1;
  for (i = 0, offset -= noffset, foffset -= noffset, path -= ndict, dict -= ndict, fdict -= ndict; i < ndict; i++, dict++, fdict++) {
    search(w, Idxseek, idxextent, idxdict, nw);
    offset++;
    foffset++;
    if (dict->nsyn > 0) {
      search(w, Synseek, synextent, syndict, nw);
      offset++;
      foffset++;
    }
  }
}

int main(int argc, char **argv) {
  u16int pageoffb[Pagesize + 1];
  pageoff = pageoffb;
  dflag = (const char **)pageoff;
  ARGBEGIN {
  case 'c':
    cflag = EARGF(usage());
    break;
  case 'd':
    *dflag = ARGF();
    if (!(*dflag)) {
      ndflag = -1;
      break;
    }
    dflag++;
    ndflag++;
    break;
  case 'a':
    aflag++;
    break;
  case 'f':
    fflag++;
    break;
  case 'l':
    lflag++;
    break;
  default:
    usage();
    break;
  } ARGEND
  dflag -= ndflag;

  char bufb[Entrysize * Pagesize] = {0};
  pagebuf = bufb;

  if (!cflag) {
      char *p = getenv("XDG_DATA_HOME"), *u = "/usr/share/stardict/dic";
      if (p == nil) {
        if ((p = getenv("HOME")) == nil)
          goto noenv;
        memcpy(p + strlen(p), "/.local/share", 14);
      }
      memcpy(p + strlen(p), "/stardict/dic", 14);

    if (chdir(p) != 0) {
    noenv:
      if (chdir(u) != 0) {
        sysfatal("chdir %s: %r", u);
      }
    } else if (nftw(u, filter, 16, FTW_PHYS) == -1)
      if (errno != ENOENT)
        sysfatal("nftw: %r");
  } else if (chdir(cflag) == -1)
    sysfatal("chdir %s: %r", cflag);

  if (nftw(".", filter, 16, FTW_PHYS) == -1)
    sysfatal("nftw: %r");
  readifo();

  Offset offsetb[noffset];
  int foffsetb[noffset];
  int fdictb[ndict];
  offset = offsetb;
  foffset = foffsetb;
  fdict = fdictb;
  /*
    very low stack size?
    off = malloc(noff*sizeof(Off));
    foff = malloc(noff*sizeof(int));
    fdict = malloc(ndict*sizeof(int));
  */
  Extent extentb[Extentsize];
  extent = extentb;

  size_t i;
  for (i = 0, path -= ndict, dict -= ndict; i < ndict; i++, path++, dict++, fdict++) {
    /* base.idx\0off */
    /* in readoff first open .idx, then write '.' to \0, open .idx.off */
    memcpy(*path + dict->npath - 3, "idx\0off", 8);
    readoff(dict->nidx, Idxseek);
    if (dict->nsyn) {
      memcpy(*path + dict->npath - 3, "syn", 4);
      readoff(dict->nsyn, Synseek);
    }
    memcpy(*path + dict->npath - 3, "dict", 5);
    // if ((*fdict = open(*path, O_RDONLY)) != -1)
    //     break;
    // if (errno != ENOENT)
    //     goto err;

    // memcpy(*path + dict->npath + 1, ".dz", 4);
    if ((*fdict = open(*path, O_RDONLY)) == -1)
    // err:
      sysfatal("open %s: %r", *path);
  }

  if (argc != 0) {
    for (i = 0; i < (unsigned)argc; i++)
      sdb(argv[i]);
    return 0;
  }

  /* can-case-fold-unicodes are 2 bytes long */
  char w[2 * Wordsize];
  s16int n;
  while ((n = read(0, w, sizeof w)) > 0) {
    *(w + n - 1) = 0; /* newline */
    sdb(w);
  }
  if (n < 0)
    sysfatal("read stdin: %r");
}
