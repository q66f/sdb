// https://stardict-4.sourceforge.net/StarDictFileFormat
// https://man.9front.org/2/rune

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <unistd.h>

#include "utf/utf.h"
#include "dictzip.h"
#include "sdb.h"

#define nil nullptr
#define isupper(r) ((unsigned)(r) - 'A' < 26)

enum {
  // sdcv ENTR_PER_PAGE = 32, after some calc, 64 is better
  // .idx <=255 word| \0| 4 .dict offset| 4 .dict word size
  // .syn <=255 word| \0| 4 nth word in .idx
  ENTR_PER_PAGE = 64,
  MAX_PER_WORD = 256,
  MAX_PER_ENTRY = 264,
  IDX_TRAILER = 9,
  SYN_TRAILER = 5,
};

typedef struct Map Map;
typedef struct Dictionary Dictionary;
typedef struct Match Match;

struct Map {
  const u8 *map;
  size_t map_size;
  u32 *page_offsets; // offsets to pages starts in index file
  u32 page_count; // equals to 2 + (word_count-1) / ENTR_PER_PAGE, 2 for 0 and offset of last word
  u32 word_count;
};

struct Dictionary {
  Map idx;
  Map syn;

  union {
    struct {
      const u8 *map;
      size_t map_size;
    };
    Dictzip *dz;
  } dict;

  char *path;
  u32 path_len; // length of base.off

  u8 type_sequence; // sametypesequence
  bool is_dz;
};

struct Match {
  u32 offset;
  u32 size;
};

static Dictionary *dictionaries;
static u32 dictionary_count, dictionary_cap, dictionary_index;

static Match *matches;
static u32 match_count, match_cap;

static const char *base_dir;
static const char **target_dictionary_names;
static u32 target_dictionary_count;

static bool accurate_accent;
static bool list_dictionaries;
static bool strict_case;
static bool unicode_fold;

static char page_buf[MAX_PER_ENTRY * ENTR_PER_PAGE];

static const char latin1[] =
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


[[noreturn]] static void
sysfatal(char *f, ...) {
	int err = errno;
	va_list ap;

	if(argv0){
		struct iovec v[] = {{argv0, strlen(argv0)}, {": ", 2}};
		writev(2, v, 2);
	}

	va_start(ap, f);
	vdprintf(2, f, ap);
	va_end(ap);

	if(f[strlen(f) - 1] == ' '){
		char *s = strerror(err);
		struct iovec v[] = {{s, strlen(s)}, {"\n", 1}};
		writev(2, v, 2);
  } else {
    write(2, "\n", 1);
  }
  exit(1);
}

static void usage(void) {
  sysfatal("usage: sdb [-c dir] [-d dict] [-alsu] [--] [pattern]\n"
           "    -c dir  Use dictionaries in given dir.\n"
           "    -d dict Use the given dictionary.\n"
           "    -a      Accurate accent matching (disable Latin-1 accent folding).\n"
           "    -l      Print a list of available dictionaries.\n"
           "    -s      Strict case matching (disable case-insensitive search).\n"
           "    -u      Unicode case folding.\n"
           "    --      Stop option parsing.\n");
}

static inline bool match_name(const char *v, u16 nv) {
	for (u32 i = 0; i < target_dictionary_count; i++)
		if (strncmp(target_dictionary_names[i], v, nv) == 0 && target_dictionary_names[i][nv] == '\0')
			return true;
	return false;
}

static inline u32 parse_u32(const u8 *p, u16 len) {
	u32 res = 0;
	for (u16 i = 0; i < len && p[i] >= '0' && p[i] <= '9'; i++) {
		res = res * 10 + (u32)(p[i] - '0');
	}
	return res;
}

static int
walk_and_identify(const char *path, const struct stat *st, int type, struct FTW *ftw) {
	if (ftw->level > 3) return (type == FTW_D) ? FTW_SKIP_SUBTREE : 0;
	if (type != FTW_F) return 0;

	size_t path_len = strlen(path);
	if (path_len < 4 || memcmp(path + path_len - 4, ".ifo", 4) != 0) return 0;

	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;

	u8 *map = mmap(nil, (size_t)st->st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) return 0;

	u8 *ptr = map;
	u8 *limit = map + st->st_size;
	u8 *line = (u8 *)memchr(map, '\n', (size_t)st->st_size);
	if (!line) { munmap(map, (size_t)st->st_size); return 0; }
	ptr = line + 1;

	u32 wc = 0, sc = 0;
	u8 ts = 0;
	bool keep = (target_dictionary_count == 0);

	while (ptr < limit) {
		u8 *eq = memchr(ptr, '=', (size_t)(limit - ptr));
		if (!eq) break;

		u8 *eol = memchr(eq, '\n', (size_t)(limit - eq));
		if (!eol) eol = limit;

		u8 *val = eq + 1;
		u16 nv = (u16)((eol > ptr && eol[-1] == '\r') ? (eol - eq - 2) : (eol - eq - 1));

		if (memcmp(ptr, "bookname", 8) == 0) {
			if (target_dictionary_count > 0) keep = match_name((char *)val, nv);
			if (list_dictionaries) write(1, val, nv);
		} else if (memcmp(ptr, "wordcount", 9) == 0) {
			wc = parse_u32(val, nv);
		} else if (memcmp(ptr, "synwordcount", 12) == 0) {
			sc = parse_u32(val, nv);
		} else if (memcmp(ptr, "sametypesequence", 16) == 0) {
			ts = (isupper(*val) || nv > 1) ? 0 : 1;
		} else if (memcmp(ptr, "idxoffsetbits", 13) == 0) {
        if (memcmp(val, "64", 2) == 0)
          sysfatal("idxoffsetbits=64 unsupported: %s", path);
    }

		ptr = eol + 1;
	}
	munmap(map, (size_t)st->st_size);

	if (keep) {
		if (dictionary_count >= dictionary_cap) {
			dictionary_cap = dictionary_cap ? dictionary_cap * 2 : 16;
			dictionaries = realloc(dictionaries, dictionary_cap * sizeof(Dictionary));
		}
		Dictionary *d = &dictionaries[dictionary_count++];
		memset(d, 0, sizeof(Dictionary));
		d->path = malloc(path_len + 5); // 5 for .idx.off\0 or .dict.dz\0 minus .ifo
		memcpy(d->path, path, path_len + 1);
		d->path_len = (u32)path_len;
		d->idx.word_count = wc;
		d->syn.word_count = sc;
		d->type_sequence = ts;
	}
	if (list_dictionaries)
    printf("\t%u\t%u\n", wc, sc);

	return 0;
}

static void
identify_dictionaries(void) {
	if (base_dir && nftw(base_dir, walk_and_identify, 16, FTW_PHYS) == -1)
		sysfatal("nftw %s: ", base_dir);

	const char *xdg = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	if (xdg) {
		snprintf(page_buf, sizeof(page_buf), "%s/stardict/dic", xdg);
		nftw(page_buf, walk_and_identify, 16, FTW_PHYS);
	} else if (home) {
		snprintf(page_buf, sizeof(page_buf), "%s/.local/share/stardict/dic", home);
		nftw(page_buf, walk_and_identify, 16, FTW_PHYS);
	}
	nftw("/usr/share/stardict/dic", walk_and_identify, 16, FTW_PHYS);

	if (list_dictionaries) exit(0);
	if (dictionary_count == 0) sysfatal("no dictionaries found");
}

static void
load_index(Map *m, u8 trailer) {
	struct stat st, ost;
	Dictionary *d = &dictionaries[dictionary_index];

	int fd = open(d->path, O_RDONLY);
	if(fd < 0) sysfatal("open %s: ", d->path);
	fstat(fd, &st);

	m->map_size = (size_t)st.st_size;
	m->map = mmap(nil, m->map_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if(m->map == MAP_FAILED) sysfatal("mmap %s: ", d->path);
	close(fd);

	m->page_count = 2 + (m->word_count - 1) / ENTR_PER_PAGE;
	size_t off_sz = m->page_count * sizeof(u32);
	
	d->path[d->path_len] = '.'; // base.idx.off

  int ofd = open(d->path, O_RDONLY);
	if(ofd >= 0 && fstat(ofd, &ost) == 0 && ost.st_size == (off_t)off_sz && ost.st_mtime >= st.st_mtime){
		m->page_offsets = mmap(nil, off_sz, PROT_READ, MAP_PRIVATE, ofd, 0);
		close(ofd);
		if(m->page_offsets != MAP_FAILED) return;
	}
	if(ofd >= 0) close(ofd);

	if((ofd = open(d->path, O_RDWR|O_CREAT|O_TRUNC, 0644)) >= 0){
		ftruncate(ofd, (long)off_sz);
		m->page_offsets = mmap(nil, off_sz, PROT_READ|PROT_WRITE, MAP_SHARED, ofd, 0);
		close(ofd);
	} else {
		m->page_offsets = mmap(nil, off_sz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	}
	if(m->page_offsets == MAP_FAILED) sysfatal("mmap %s: ", d->path);

	m->page_offsets[0] = 0;
	const u8 *ptr = m->map;
	for(u32 i = 1; i < m->page_count; i++){
		u32 step = (i == m->page_count - 1) ? ((m->word_count - 1) % ENTR_PER_PAGE) : ENTR_PER_PAGE;
		for(u32 j = 0; j < step; j++)
			ptr = (u8 *)strchr((char *)ptr, '\0') + trailer;
		m->page_offsets[i] = (u32)(ptr - m->map);
	}
}

static void initialize_dictionaries(void) {
  if (list_dictionaries)
    write(1, "Dictionary's name   Word count   Synonom count\n", 47);

  identify_dictionaries();
  for (dictionary_index = 0; dictionary_index < dictionary_count;
       dictionary_index++) {
    Dictionary *d = &dictionaries[dictionary_index];

    memcpy(d->path + d->path_len - 3, "idx\0off", 8);
    load_index(&d->idx, IDX_TRAILER);

    if (d->syn.word_count) {
      memcpy(d->path + d->path_len - 3, "syn", 4);
      load_index(&d->syn, SYN_TRAILER);
    }

    memcpy(d->path + d->path_len - 3, "dict", 5);
    int fd = open(d->path, O_RDONLY);
    if (fd >= 0) {
      struct stat st;
      fstat(fd, &st);
			d->dict.map_size = (size_t)st.st_size;
			d->dict.map = mmap(nil, d->dict.map_size, PROT_READ, MAP_PRIVATE, fd, 0);
			close(fd);
			d->is_dz = false;
		} else {
		  if(errno != ENOENT) sysfatal("open dict %s: ", d->path);
			memcpy(d->path + d->path_len + 1, ".dz", 4);
			Dictzip *dz = dzopen(d->path);
			if (!dz) sysfatal("data file missing for %s", d->path);
			d->dict.dz = dz;
			d->is_dz = true;
		}
	}
}

// https://github.com/projg2/portable-endianness
static inline u32 read_u32_be(const u8 *p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static inline void 
save_match(const char *ptr, u32 n, bool is_syn) 
{
	if (match_count >= match_cap) {
		match_cap = match_cap ? match_cap * 2 : 16;
		matches = realloc(matches, match_cap * sizeof(Match));
	}

	if (is_syn) {
		matches[match_count++].size = read_u32_be((u8 *)ptr + n);
	} else {
		struct iovec iov[2] = {{(void *)ptr, (size_t)n-1}, {" ", 1}};
		writev(1, iov, 2);

		matches[match_count].offset = read_u32_be((u8 *)ptr + n);
		matches[match_count++].size = read_u32_be((u8 *)ptr + n + 4);
	}
}

static inline int word_cmp(const char *w, const char *p, u32 n) {
  int r = strncasecmp(w, p, n);
  return (r == 0 && unicode_fold) ? memcmp(w, p, n) : r;
}

static void 
flush_matches(bool is_syn) 
{
	Dictionary *d = &dictionaries[dictionary_index];
	for (u32 i = 0; i < match_count; i++) {
		const char *data;
		u32 sz, off;

		if (is_syn) {
			Map *m = &d->idx;
			u32 widx = matches[i].size;
			const char *p = (const char *)(m->map + m->page_offsets[widx / ENTR_PER_PAGE]);

			for (u32 j = 0; j < (widx % ENTR_PER_PAGE); j++)
				p = strchr(p, '\0') + IDX_TRAILER;

			size_t len = strlen(p);
			struct iovec iov[2] = { {(void *)p, len}, {"\n", 1} };
			writev(1, iov, 2);

			const u8 *tr = (u8 *)p + len + 1;
			off = read_u32_be(tr);
			sz  = read_u32_be(tr + 4);
		} else {
			off = matches[i].offset;
			sz  = matches[i].size;
		}

		if (d->is_dz) {
			dzread(d->dict.dz, page_buf, sz, off);
			data = page_buf;
		} else {
			data = (const char *)d->dict.map + off;
		}

  // half way
  // https://github.com/ianlewis/go-stardict/blob/master/dict/dict.go
  u8 seek;
  if (d->type_sequence == 1) {
    seek = 0;
  } else {
    if (isupper(d->type_sequence))
      sysfatal("%s sametypesequence type unsupported", d->path);
    seek = 1;
    sz--; // null terminator
  }

  // adding html support is easy, just how to do it good enough
		struct iovec iov[2] = { {(void *)(data + seek), sz - seek}, {"\n", 1} };
		writev(1, iov, 2);
	}
	match_count = 0;
}

static void 
search_map(const char *word, u32 nw, Map *m, u8 trailer, bool is_syn)
{
	u32 low = 0, high = m->page_count - 1, mid;
	u32 nwseek = nw - 1 + trailer;

	while (low <= high) {
		mid = low + (high - low) / 2;
		if (word_cmp(word, (const char *)m->map + m->page_offsets[mid], nw) >= 0) {
			low = mid + 1;
		} else {
			if (mid == 0) break;
			high = mid - 1;
		}
	}
	u32 pg = (high >= m->page_count) ? m->page_count - 1 : high;

	const char *ptr = (const char *)(m->map + m->page_offsets[pg]);
	const char *page_limit = (pg == m->page_count - 1) 
	                         ? (const char *)(m->map + m->map_size) 
	                         : (const char *)(m->map + m->page_offsets[pg + 1]);

	while (ptr < page_limit) {
		int ret = word_cmp(word, ptr, nw);
		if (ret == 0) {
			const char *fwd = ptr;
			while ((size_t)((const char *)m->map + m->map_size - fwd) > nwseek && word_cmp(word, fwd, nw) == 0) {
				save_match(fwd, nw, is_syn);
				fwd += nwseek;
			}

			const char *bak = ptr;
			while ((size_t)(bak - (const char *)m->map) >= nwseek) {
				bak -= nwseek;
				if (word_cmp(word, bak, nw) != 0) break;
				save_match(bak, nw, is_syn);
			}
			flush_matches(is_syn);
			return;
		};
		if (ret < 0) return;
		ptr = strchr(ptr, '\0') + trailer;
	}
}

static inline void accent_fold(char *s)
{
    Rune r;
    char *rp = s, *wp = s;
    while (*rp) {
        int n = chartorune(&r, rp);
        if (r >= 0xc0 && r <= 0xff && latin1[r - 0xc0]) {
            *wp++ = latin1[r - 0xc0];
            rp += n;
        } else {
            if (wp != rp) {
              for (int i = 0; i < n; i++) *wp++ = *rp++;
            } else {
                wp += n; rp += n;
            }
        }
    }
    *wp = '\0';
}

static inline void unicode_lower(char *cp) {
  Rune r;

  while (*cp) {
    chartorune(&r, cp);
    r = tolowerrune(r);
    cp += runetochar(cp, &r);
  }
}

static inline void lookup(char *word) {
  if (!accurate_accent) accent_fold(word);
  if (unicode_fold) unicode_lower(word);

  u16 nw = (u16)strlen(word) + 1;
  for (dictionary_index = 0; dictionary_index < dictionary_count; dictionary_index++) {
    search_map(word, nw, &dictionaries[dictionary_index].idx, IDX_TRAILER, false);
    if (dictionaries[dictionary_index].syn.word_count) {
      search_map(word, nw, &dictionaries[dictionary_index].syn, SYN_TRAILER, true);
    }
  }
}

int main(int argc, char **argv) {
  static const char *targets[256];
	target_dictionary_names = targets;

  ARGBEGIN {
  case 'c': base_dir = EARGF(usage()); break;
  case 'd': if ((target_dictionary_names[target_dictionary_count] = ARGF())) target_dictionary_count++; break;
  case 'a': accurate_accent = true; break;
  case 'l': list_dictionaries = true; break;
  case 's': strict_case = true; break;
  case 'u': unicode_fold = true; break;
  default: usage(); break;
  } ARGEND

  initialize_dictionaries();

  if (argc > 0) {
    for (int i = 0; i < argc; i++) lookup(argv[i]);
  } else {
    char line[MAX_PER_WORD * 2];
    while (fgets(line, sizeof(line), stdin)) {
      line[strcspn(line, "\r\n")] = 0;
      if (*line) lookup(line);
    }
  }
  return 0;
}
