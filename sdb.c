/* https://stardict-4.sourceforge.net/StarDictFileFormat */
/* https://man.9front.org/2/rune */

#define nil NULL
#include "sdb.h"

#define islatin1(r) ((unsigned)(r) - 0xc0 < 0x40)
#define isupper(r) ((unsigned)(r) - 'A' < 26)

static const char *cflag;
static const char **dflag;
static s8int ndflag;
static bool aflag;
static bool fflag;
static bool lflag;

typedef struct Dict Dict;
typedef struct Off Off;
typedef struct Key Key;

struct Dict {
  char *path;
  u8int npath; /* length of base.off */
  u8int ntype; /* sametypesequence */
  u32int nidx; /* .idx word count */
  u32int nsyn; /* .syn word count */
};

struct Off {
  u32int npage; /* total page count, equals to 2+(wordcount-1)/Pagesize, 2 for 0 and offset of last word */
  u32int *off;  /* offset to page begining */
};

struct Key {
  u32int off;
  u32int size;
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
  Keysize = 8, /* dynamic array? */
};

static Dict *dict;
static int *fdict;  /* fd */
static u8int ndict; /* matched dict number */

static Off *off;
static int *foff; /* fd */
static u8int noff; /* essentially the number of matched idx (equal to ndict) and syn */

static Key *key;
static u8int nkey; /* matched key number */

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

static void cd(void) {
  if (!cflag) {
    char *p = getenv("XDG_DATA_HOME");
    if (p == nil) {
      p = getenv("HOME");
      if (p == nil)
        goto noenv;
      memcpy(p + strlen(p), "/.local/share", 14);
    }
    memcpy(p + strlen(p), "/stardict/dic", 14);
    if (chdir(p) == 0)
      return;
  noenv:
    cflag = "/usr/share/stardict/dic";
  }
  if (chdir(cflag) == -1)
    sysfatal("chdir %s: %r", cflag);
}

static int fil(const struct dirent *p) {
  return !memcmp(p->d_name + strlen(p->d_name) - 4, ".ifo", 4);
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
  u8int n;
  int i, fd;
  struct dirent **p;
  struct stat st;
  char *buf, *tem, *val;
  u16int nval;

  if (!(n = scandir(".", &p, fil, alphasort)))
    sysfatal("scandir .ifo: none, %r");
  dict = malloc(n * sizeof(Dict));

  for (i = 0, buf = pagebuf; i < n; i++, dict++) {
    if ((fd = open(p[i]->d_name, O_RDONLY)) == -1)
      sysfatal("open %s: %r", p[i]->d_name);
    fstat(fd, &st);

    if (st.st_size < Entrysize * Pagesize)
      buf = calloc(1, st.st_size);

    /* St.. + lf + version + lf = 39 */
    /* to deal with crlf */
    if (pread(fd, buf, st.st_size - 38, 38) == -1 ||
        (tem = memchr(buf, '\n', 3)) == nil)
      sysfatal("inval %s\n", p[i]->d_name);

    dict->nsyn = 0;
    while (1) {
      buf = tem + 1;
      /* 17 's longest and newline's not allowed per spec */
      if ((val = memchr(buf, '=', 17 + 4)) == nil)
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

      if (memcmp(buf, "bookname", 8) == 0) {
        if (nval == 0)
          sysfatal("%s no bookname\n", p[i]->d_name);
        if (ndflag > 0 && matchdflag(val, nval) == 1) {
          dict--;
          goto nextdict;
        } else if (ndflag == -1)
          write(1, val, nval);
      } else if (memcmp(buf, "wordcount", 9) == 0) {
        dict->nidx = atol(val);
        if (dict->nidx < Pagesize)
          sysfatal("%s has less 64 word\n", p[i]->d_name);
        close(fd);
      } else if (memcmp(buf, "synwordcount", 12) == 0) {
        dict->nsyn = atol(val);
        if (dict->nsyn < Pagesize)
          sysfatal("%s has less 64 word\n", p[i]->d_name);
      } else if (memcmp(buf, "sametypesequence", 16) == 0) {
        if (isupper(*val) || nval > 1)
          sysfatal("%s sametypesequence type unsupported\n", p[i]->d_name);
        dict->ntype = 1;
      } else if (memcmp(buf, "idxoffsetbits", 13) == 0)
        if (memcmp(val, "64", 2) == 0)
          sysfatal("idxoffsetbits=64 unsupported");
    }

    if (ndflag != -1) {
      dict->npath = strlen(p[i]->d_name);
      dict->path = malloc(dict->npath + 5); /* .idx.off\0 or .dict.dz\0 minus .ifo */
      memcpy(dict->path, p[i]->d_name, dict->npath - 3); /* base. without ifo */
      ndict++;
      if (dict->nsyn)
        noff++;
    } else
      printf("\t%u\t%u\n", dict->nidx, dict->nsyn);
  nextdict:
    close(fd);
    free(p[i]);
  }
  if (ndflag == -1)
    exit(0);
  noff += ndict;
  free(p);
}

static void genoff(u32int wc, struct stat st, u8int entryseek) {
  int fd;
  u32int i, seek;
  u8int j;
  char *buf;

  buf = malloc(st.st_size);
  if (read(*foff, buf, st.st_size) == -1) {
    *(dict->path + dict->npath) = 0; /* base.idx"\0"off */ 
    sysfatal("read %s: %r", dict->path);
  }

  *(off->off++) = 0;
  for (i = seek = 0; i < off->npage - 2; i++) {
    for (j = 0; j < Pagesize; j++)
      seek += strlen(buf + seek) + entryseek;
    *(off->off++) = seek;
  }

  for (i = 0; i < wc % Pagesize - 1; i++)
    seek += strlen(buf + seek) + entryseek;
  *off->off = seek;
  off->off -= off->npage - 1;

  if ((fd = creat(dict->path, 0644)) == -1 ||
      write(fd, off->off, off->npage * 4) == -1)
    sysfatal("gen %s: %r", dict->path);
  close(fd);
}

/* https://github.com/projg2/portable-endianness */
static u32int readu32be(const u8int *p) {
  return ((u32int)p[0] << 24) | ((u32int)p[1] << 16) | ((u32int)p[2] << 8) | (u32int)p[3];
}

static void readoff(u32int wc, int entryseek) {
  int fd;
  struct stat s, st;

  off->npage = 2 + (wc - 1) / Pagesize;
  off->off = malloc(off->npage * 4);

  if ((*foff = open(dict->path, O_RDONLY)) == -1)
    sysfatal("open %s: %r", dict->path);
  fstat(*foff, &st);

  *(dict->path + dict->npath) = '.'; /* base.idx'.'off */
  if (access(dict->path, F_OK) == -1)
    genoff(wc, st, entryseek);

  if ((fd = open(dict->path, O_RDONLY)) == -1)
    sysfatal("open %s: %r", dict->path);
  fstat(fd, &s);
  if (s.st_mtime < st.st_mtime)
    genoff(wc, st, entryseek);
  foff++;

  if (read(fd, off->off, s.st_size) == -1)
    sysfatal("read %s: %r", dict->path);
  close(fd);
  off++;
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
  if (pread(*foff, pagebuf, Wordsize, offset) == -1)
    sysfatal("pread %s: %r", dict->path);
  return pagebuf;
}

/* .idx |.syn page no, Idxseek |Synseek */
static void loadpage(u32int npage, u8int entryseek) {
  u32int *tem, i;
  u8int j;

  tem = off->off + npage;
  i = *tem;
  pread(*foff, pagebuf, *(tem + 1) - i, i);
  /* last page size < Pageszie, but extra bytes barely used */
  for (i = j = 0; j < Pagesize; j++, i += strlen(pagebuf + i) + entryseek)
    *(pageoff + j) = i;
}

static void idxkey(char *w, u8int nw) {
  *(w + nw - 1) = ' ';
  write(1, w, nw);

  key->off = readu32be((u8int *)w + nw);
  key->size = readu32be((u8int *)w + nw + 4);
  nkey++;
  key++;
}

static void synkey(char *w, u8int nw) {
  key->size = readu32be((u8int *)w + nw);
  nkey++;
  key++;
}

/* half way */
/* https://github.com/ianlewis/go-stardict/blob/master/dict/dict.go */
static void idxdict(void) {
  u8int i, typeseek;

  /* key-=nkey for match idxkey()'s print order */
  for (i = 0, key -= nkey; i < nkey; i++, key++) {
    if (key->size + 1 > Entrysize * Pagesize) /* newline */
      pagebuf = malloc(key->size + 1);
    pread(*fdict, pagebuf, key->size, key->off);

    if (dict->ntype == 1) {
      typeseek = 0;
    } else {
      if (isupper(*pagebuf))
        sysfatal("%s sametypesequence type unsupported\n", dict->path);
      typeseek = 1;
      key->size--; /* null terminator */
    }
    /* adding html support is easy, just how to do it good enough */
    *(pagebuf + key->size) = '\n';
    write(1, pagebuf + typeseek, key->size - typeseek + 1);
  }
  key -= nkey;
  nkey = 0;
}

static void syndict(void) {
  u8int i;
  u32int *tem, j;
  char *w;

  for (i = 0, key -= nkey; i < nkey; i++, key++) {
    tem = (off - 1)->off + (key->size / Pagesize);
    j = *tem;
    /* foff - 1 is .idx, */
    pread(*(foff - 1), pagebuf, *(tem + 1) - j, j);
    w = pagebuf;
    for (j = 0; j < key->size % Pagesize; j++, w += strlen(w) + Idxseek)
      ;

    j = strlen(w) + 1;
    *(w + j - 1) = '\n';
    write(1, w, j);

    key->off = readu32be((u8int *)w + j);
    key->size = readu32be((u8int *)w + j + 4);
  }
  idxdict();
}

/* boundless_binary_search */
/* https://github.com/scandum/binary_search/blob/master/binary_search.c */
/*
   search 1st word of each page,
   if the 1st word matched the the word we searching
     if doing fuzzy matching (these might be multiple keys) check at least 4 words back and forth.
     if not doing fuzzy matching, flush and return
   if its not matched, search the final page that the word may located at
   then if matched and fuzzy matching
   loop the whole page back and forth until failure or continue looping adjacent page
*/
/* entryseek is Idxseek or Synseek */
static void search(char *w, int entryseek, void (*savekey)(char *, u8int),
            void (*dictflush)(void), u8int nw) {
  u32int bot, mid, boo, mii;
  int res;
  char *buf, *mnt;
  u16int nwseek = nw - 1 + entryseek;

  if (cmp(w, getword(0), nw) < 0 ||
      cmp(w, getword(*(off->off + off->npage - 1)), nw) > 0)
    return;

  bot = 0;
  mid = off->npage - 1; /* excluding the end word offset */
  while (mid > 1) {
    res = cmp(w, getword(*(off->off + bot + mid / 2)), nw);
    if (res > 0)
      bot += mid++ / 2;
    else if (res == 0) {
      bot += mid / 2;
      goto page1stmatched;
    }
    mid /= 2;
  }

  res = cmp(w, getword(*(off->off + bot)), nw);
  if (res < 0)
    bot--;
  else if (res == 0) {
  page1stmatched:
    savekey(pagebuf, nw);
    if (!fflag) {
      if ((res = 4 * nwseek - Wordsize) > 0)
        pread(*foff, pagebuf + Wordsize, res, *(off->off + bot) + Wordsize);

      for (buf = pagebuf + nwseek; nkey < 4; buf += nwseek)
        if (cmp(w, buf, nw) == 0)
          savekey(buf, nw);
        else
          break;
      pread(*foff, buf, 4 * nwseek, *(off->off + bot) - 4 * nwseek);
      for (; nkey < 8; buf += nwseek)
        if (cmp(w, buf, nw) == 0)
          savekey(buf, nw);
        else
          break;
    }
    dictflush();
    return;
  }

  loadpage(bot, entryseek);

  /* not at the last page || page wordcount equals to Pagesize */
  mid = mii = (bot != off->npage - 2 || dict->nidx % Pagesize == 0)
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
      savekey(buf, nw);
      if (!fflag) {
        mii = boo = boo + 1;
      pageloop:
        for (; mii < mid + 1; mii++)
          if (cmp(w, mnt += nwseek, nw) == 0)
            savekey(mnt, nw);
          else
            break;
        for (; boo > 1; boo--)
          if (cmp(w, buf -= nwseek, nw) == 0)
            savekey(buf, nw);
          else
            break;
        if (mii == mid + 1 && bot + 1 < off->npage) {
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
      if (nkey > Keysize)
        nkey = Keysize;
      dictflush();
      return;
    }
    mii /= 2;
  }
  if (cmp(w, buf = pagebuf + *(pageoff + boo), nw) == 0) {
    savekey(buf, nw);
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
  for (i = 0, off -= noff, foff -= noff, dict -= ndict, fdict -= ndict; i < ndict; i++, dict++, fdict++) {
    search(w, Idxseek, idxkey, idxdict, nw);
    off++;
    foff++;
    if (dict->nsyn > 0) {
      search(w, Synseek, synkey, syndict, nw);
      off++;
      foff++;
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
    aflag++; /* suppress accent fold */
    break;
  case 'f':
    fflag++; /* suppress fuzzy ascii case */
    break;
  case 'l':
    lflag++; /* unicode case fold */
    break;
  default:
    usage();
    break;
  } ARGEND
  dflag -= ndflag;

  char bufb[Entrysize * Pagesize] = {0};
  pagebuf = bufb;

  cd();
  readifo();

  Off offb[noff];
  int foffb[noff];
  int fdictb[ndict];
  off = offb;
  foff = foffb;
  fdict = fdictb;
  /*
    very low stack size?
    off = malloc(noff*sizeof(Off));
    foff = malloc(noff*sizeof(int));
    fdict = malloc(ndict*sizeof(int));
  */
  Key keyb[Keysize];
  key = keyb;

  int i;
  for (i = 0, dict -= ndict; i < ndict; i++, dict++, fdict++) {
    /* base.idx\0off */
    /* in readoff first open .idx, then write '.' to \0, open .idx.off */
    memcpy(dict->path + dict->npath - 3, "idx\0off", 8);
    readoff(dict->nidx, Idxseek);
    if (dict->nsyn) {
      memcpy(dict->path + dict->npath - 3, "syn", 4);
      readoff(dict->nsyn, Synseek);
    }
    memcpy(dict->path + dict->npath - 3, "dict", 5);
    // if ((*fdict = open(dict->path, O_RDONLY)) != -1)
    //     break;
    // if (errno != ENOENT)
    //     goto err;

    // memcpy(dict->path + dict->npath + 1, ".dz", 4);
    if ((*fdict = open(dict->path, O_RDONLY)) == -1)
    // err:
      sysfatal("open %s: %r", dict->path);
  }

  if (argc != 0) {
    for (i = 0; i < argc; i++)
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
