/* ssdeep
 * Copyright (C) 2002 Andrew Tridgell <tridge@samba.org>
 * Copyright (C) 2006 ManTech International Corporation
 * Copyright (C) 2013 Helmut Grohne <helmut@subdivi.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
  printf ("fuzzyhashfile\n");
 *
 * Earlier versions of this code were named fuzzy.c and can be found at:
 *     http://www.samba.org/ftp/unpacked/junkcode/spamsum/
 *     http://ssdeep.sf.net/
 */

#include "main.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "fuzzy.h"
#include "edit_dist.h"

#if defined(__GNUC__) && __GNUC__ >= 3
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define likely(x) x
#define unlikely(x) x
#endif

#define ROLLING_WINDOW 7
#define MIN_BLOCKSIZE 3
#define HASH_PRIME 0x01000193
#define HASH_INIT 0x28021967
#define NUM_BLOCKHASHES 31

#define EDIT_DISTN_MAXLEN 64 /* MAX_SPAMSUM */
#define EDIT_DISTN_INSERT_COST 1
#define EDIT_DISTN_REMOVE_COST 1
#define EDIT_DISTN_REPLACE_COST 2

//#define MIN(x,y) ((x)<(y)?(x):(y))

int area_distx(char* a)
{
    int i = 0;
    int bottom[10] = {0};
    int x, y, x1, y1, area, k, j;

    area = 0;

    for (i=0 ; i < 15 ; i++)
    {
        x = a[2*i] - 48;
        y = a[2*i + 1] - 48;

        x1 = a[2*i + 2] - 48;
        y1 = a[2*i + 3] - 48;

        k = x - x1;
        j = y - y1;

        if (bottom[MIN(x1, x)] >= MIN(y1, y))
        {
            continue;
        }
        if (k*j != 0)
        {
            area = area + MIN(y1, y) - bottom[MIN(x1, x)] + 0.5;
            bottom[MIN(x1, x)] = bottom[MIN(x1, 1)] + MIN(y1, y) + 0.5;
        }
        if (j == 0 && k != 0)
        {
            area = area + y - bottom[MIN(x1, x)];
            bottom[MIN(x1, x)] += y;
        }
    }

    return area;
}

int area_disty(char* a)
{
    int i = 0;
    int bottom[10] = {0};
    int x, y, x1, y1, area, k, j;

    area = 0;

    for (i=0 ; i < 15 ; i++)
    {
        x = a[2*i] - 48;
        y = a[2*i + 1] - 48;

        x1 = a[2*i + 2] - 48;
        y1 = a[2*i + 3] - 48;

        k = x - x1;
        j = y - y1;

        if (bottom[MIN(y1, y)] >= MIN(x1, x))
        {
            continue;
        }
        if (k*j != 0)
        {
            area = area + MIN(x1, x) - bottom[MIN(y1, y)] + 0.5;
            bottom[MIN(y1, y)] = bottom[MIN(y1, 1)] + MIN(x1, x) + 0.5;
        }
        if (j != 0 && k == 0)
        {
            area = area + y - bottom[MIN(y1, y)];
            bottom[MIN(x1, x)] += y;
        }
    }

    return area;
}

int area_dist(int a1, int a2, int b1, int b2)
{
    int result;
    int a, b;

    a = a1 - a2;
    b = b1 - b2;

    a = a * a;
    b = b * b;

    result = sqrt(a+b);
}


struct roll_state {
  unsigned char window[ROLLING_WINDOW];
  uint32_t h1, h2, h3;
  uint32_t n;
};

static void roll_init(/*@out@*/ struct roll_state *self) {
	memset(self, 0, sizeof(struct roll_state));
}

/*
 * a rolling hash, based on the Adler checksum. By using a rolling hash
 * we can perform auto resynchronisation after inserts/deletes

 * internally, h1 is the sum of the bytes in the window and h2
 * is the sum of the bytes times the index

 * h3 is a shift/xor based rolling hash, and is mostly needed to ensure that
 * we can cope with large blocksize values
 */
static void roll_hash(struct roll_state *self, unsigned char c)
{
  self->h2 -= self->h1;
  self->h2 += ROLLING_WINDOW * (uint32_t)c;

  self->h1 += (uint32_t)c;
  self->h1 -= (uint32_t)self->window[self->n];

  self->window[self->n] = c;
  self->n++;
  if (self->n == ROLLING_WINDOW)
    self->n = 0;

  /* The original spamsum AND'ed this value with 0xFFFFFFFF which
   * in theory should have no effect. This AND has been removed
   * for performance (jk) */
  self->h3 <<= 5;
  self->h3 ^= c;
}

static uint32_t roll_sum(const struct roll_state *self)
{
  return self->h1 + self->h2 + self->h3;
}

/* A simple non-rolling hash, based on the FNV hash. */
static uint32_t sum_hash(unsigned char c, uint32_t h)
{
  return (h * HASH_PRIME) ^ c;
}

/* A blockhash contains a signature state for a specific (implicit) blocksize.
 * The blocksize is given by SSDEEP_BS(index). The h and halfh members are the
 * FNV hashes, where halfh stops to be reset after digest is SPAMSUM_LENGTH/2
 * long. The halfh hash is needed be able to truncate digest for the second
 * output hash to stay compatible with ssdeep output. */
struct blockhash_context
{
  uint32_t h, halfh;
  char digest[SPAMSUM_LENGTH];
  char halfdigest;
  unsigned int dindex;
};

struct fuzzy_state
{
  uint_least64_t total_size;
  uint_least64_t fixed_size;
  unsigned int bhstart, bhend, bhendlimit;
  unsigned int flags;
  uint32_t lasth;
  struct blockhash_context bh[NUM_BLOCKHASHES];
  struct roll_state roll;
};

#define FUZZY_STATE_NEED_LASTHASH  1u
#define FUZZY_STATE_SIZE_FIXED     2u

#define SSDEEP_BS(index) (((uint32_t)MIN_BLOCKSIZE) << (index))
#define SSDEEP_TOTAL_SIZE_MAX \
  ((uint_least64_t)SSDEEP_BS(NUM_BLOCKHASHES-1) * SPAMSUM_LENGTH)

/*@only@*/ /*@null@*/ struct fuzzy_state *fuzzy_new(void)
{
  struct fuzzy_state *self;
  if(NULL == (self = malloc(sizeof(struct fuzzy_state))))
    /* malloc sets ENOMEM */
    return NULL;
  self->bhstart = 0;
  self->bhend = 1;
  self->bhendlimit = NUM_BLOCKHASHES - 1;
  self->bh[0].h = HASH_INIT;
  self->bh[0].halfh = HASH_INIT;
  self->bh[0].digest[0] = '\0';
  self->bh[0].halfdigest = '\0';
  self->bh[0].dindex = 0;
  self->total_size = 0;
  self->flags = 0;
  roll_init(&self->roll);
  return self;
}

/*@only@*/ /*@null@*/ struct fuzzy_state *fuzzy_clone(const struct fuzzy_state *state)
{
  struct fuzzy_state *newstate;
  if (NULL == (newstate = malloc(sizeof(struct fuzzy_state))))
    /* malloc sets ENOMEM */
    return NULL;
  memcpy(newstate, state, sizeof(struct fuzzy_state));
  return newstate;
}

int fuzzy_set_total_input_length(struct fuzzy_state *state, uint_least64_t total_fixed_length)
{
  unsigned int bi = 0;
  if (total_fixed_length > SSDEEP_TOTAL_SIZE_MAX)
  {
    errno = EOVERFLOW;
    return -1;
  }
  if ((state->flags & FUZZY_STATE_SIZE_FIXED) &&
      state->fixed_size != total_fixed_length)
  {
    errno = EINVAL;
    return -1;
  }
  state->flags |= FUZZY_STATE_SIZE_FIXED;
  state->fixed_size = total_fixed_length;
  while ((uint_least64_t)SSDEEP_BS(bi) * SPAMSUM_LENGTH < total_fixed_length)
  {
    ++bi;
    if (bi == NUM_BLOCKHASHES - 2)
      break;
  }
  ++bi;
  state->bhendlimit = bi;
  return 0;
}


static void fuzzy_try_fork_blockhash(struct fuzzy_state *self)
{
  struct blockhash_context *obh, *nbh;
  assert(self->bhend > 0);
  obh = self->bh + (self->bhend - 1);
  if (self->bhend <= self->bhendlimit)
  {
    nbh = obh + 1;
    nbh->h = obh->h;
    nbh->halfh = obh->halfh;
    nbh->digest[0] = '\0';
    nbh->halfdigest = '\0';
    nbh->dindex = 0;
    ++self->bhend;
  }
  else if (self->bhend == NUM_BLOCKHASHES &&
           !(self->flags & FUZZY_STATE_NEED_LASTHASH))
  {
    self->flags |= FUZZY_STATE_NEED_LASTHASH;
    self->lasth = obh->h;
  }
}

static void fuzzy_try_reduce_blockhash(struct fuzzy_state *self)
{
  assert(self->bhstart < self->bhend);
  if (self->bhend - self->bhstart < 2)
    /* Need at least two working hashes. */
    return;
  if (self->total_size <= SSDEEP_TOTAL_SIZE_MAX &&
      (uint_least64_t)SSDEEP_BS(self->bhstart) * SPAMSUM_LENGTH >=
      ((self->flags & FUZZY_STATE_SIZE_FIXED) ? self->fixed_size : self->total_size))
    /* Initial blocksize estimate would select this or a smaller
     * blocksize. */
    return;
  if (self->bh[self->bhstart + 1].dindex < SPAMSUM_LENGTH / 2)
    /* Estimate adjustment would select this blocksize. */
    return;
  /* At this point we are clearly no longer interested in the
   * start_blocksize. Get rid of it. */
  ++self->bhstart;
}

static const char *b64 =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void fuzzy_engine_step(struct fuzzy_state *self, unsigned char c)
{
  size_t h;
  unsigned int i;
  /* At each character we update the rolling hash and the normal hashes.
   * When the rolling hash hits a reset value then we emit a normal hash
   * as a element of the signature and reset the normal hash. */
  roll_hash(&self->roll, c);
  h = roll_sum(&self->roll);

  for (i = self->bhstart; i < self->bhend; ++i)
  {
    self->bh[i].h = sum_hash(c, self->bh[i].h);
    self->bh[i].halfh = sum_hash(c, self->bh[i].halfh);
  }
  if (self->flags & FUZZY_STATE_NEED_LASTHASH)
    self->lasth = sum_hash(c, self->lasth);

  for (i = self->bhstart; i < self->bhend; ++i)
  {
    /* With growing blocksize almost no runs fail the next test. */
    if (likely(h % SSDEEP_BS(i) != SSDEEP_BS(i) - 1))
      /* Once this condition is false for one bs, it is
       * automatically false for all further bs. I.e. if
       * h === -1 (mod 2*bs) then h === -1 (mod bs). */
      break;
    /* We have hit a reset point. We now emit hashes which are
     * based on all characters in the piece of the message between
     * the last reset point and this one */
    if (unlikely(0 == self->bh[i].dindex)) {
      /* Can only happen 30 times. */
      /* First step for this blocksize. Clone next. */
      fuzzy_try_fork_blockhash(self);
    }
    self->bh[i].digest[self->bh[i].dindex] =
      b64[self->bh[i].h % 64];
    self->bh[i].halfdigest = b64[self->bh[i].halfh % 64];
    if (self->bh[i].dindex < SPAMSUM_LENGTH - 1) {
      /* We can have a problem with the tail overflowing. The
       * easiest way to cope with this is to only reset the
       * normal hash if we have room for more characters in
       * our signature. This has the effect of combining the
       * last few pieces of the message into a single piece
       * */
      self->bh[i].digest[++(self->bh[i].dindex)] = '\0';
      self->bh[i].h = HASH_INIT;
      if (self->bh[i].dindex < SPAMSUM_LENGTH / 2) {
	self->bh[i].halfh = HASH_INIT;
	self->bh[i].halfdigest = '\0';
      }
    } else
      fuzzy_try_reduce_blockhash(self);
  }
}

int fuzzy_update(struct fuzzy_state *self,
		 const unsigned char *buffer,
		 size_t buffer_size) {
  if (self->total_size <= SSDEEP_TOTAL_SIZE_MAX) {
    if (buffer_size > SSDEEP_TOTAL_SIZE_MAX ||
	SSDEEP_TOTAL_SIZE_MAX - buffer_size < self->total_size ) {
      self->total_size = SSDEEP_TOTAL_SIZE_MAX + 1;
    }
    else
      self->total_size += buffer_size;
  }
  for ( ;buffer_size > 0; ++buffer, --buffer_size)
    fuzzy_engine_step(self, *buffer);
  return 0;
}

static int memcpy_eliminate_sequences(char *dst,
				      const char *src,
				      int n)
{
  const char *srcend = src + n;
  assert(n >= 0);
  if (src < srcend) *dst++ = *src++;
  if (src < srcend) *dst++ = *src++;
  if (src < srcend) *dst++ = *src++;
  while (src < srcend)
    if (*src == dst[-1] && *src == dst[-2] && *src == dst[-3])
    {
      ++src;
      --n;
    } else
      *dst++ = *src++;
  return n;
}

#ifdef S_SPLINT_S
extern const int EOVERFLOW;
#endif

int fuzzy_digest(const struct fuzzy_state *self,
		 /*@out@*/ char *result,
		 unsigned int flags)
{
  unsigned int bi = self->bhstart;
  uint32_t h = roll_sum(&self->roll);
  int i, remain = FUZZY_MAX_RESULT - 1; /* Exclude terminating '\0'. */
  /* Verify that our elimination was not overeager. */
  assert(bi == 0 || (uint_least64_t)SSDEEP_BS(bi) / 2 * SPAMSUM_LENGTH <
	 self->total_size);

  if (self->total_size > SSDEEP_TOTAL_SIZE_MAX) {
    /* The input exceeds data types. */
    errno = EOVERFLOW;
    return -1;
  }
  /* Fixed size optimization. */
  if ((self->flags & FUZZY_STATE_SIZE_FIXED) &&
      self->fixed_size != self->total_size) {
    errno = EINVAL;
    return -1;
  }
  /* Initial blocksize guess. */
  while ((uint_least64_t)SSDEEP_BS(bi) * SPAMSUM_LENGTH < self->total_size)
    ++bi;
  /* Adapt blocksize guess to actual digest length. */
  if (bi >= self->bhend)
    bi = self->bhend - 1;
  while (bi > self->bhstart && self->bh[bi].dindex < SPAMSUM_LENGTH / 2)
    --bi;
  assert (!(bi > 0 && self->bh[bi].dindex < SPAMSUM_LENGTH / 2));

  i = snprintf(result, (size_t)remain, "%lu:", (unsigned long)SSDEEP_BS(bi));
  if (i <= 0)
    /* Maybe snprintf has set errno here? */
    return -1;
  assert(i < remain);
  remain -= i;
  result += i;
  i = (int)self->bh[bi].dindex;
  assert(i <= remain);
  if ((flags & FUZZY_FLAG_ELIMSEQ) != 0)
    i = memcpy_eliminate_sequences(result, self->bh[bi].digest, i);
  else
    memcpy(result, self->bh[bi].digest, (size_t)i);
  result += i;
  remain -= i;
  if (h != 0)
  {
    assert(remain > 0);
    *result = b64[self->bh[bi].h % 64];
    if((flags & FUZZY_FLAG_ELIMSEQ) == 0 || i < 3 ||
       *result != result[-1] ||
       *result != result[-2] ||
       *result != result[-3]) {
      ++result;
      --remain;
    }
  } else if (self->bh[bi].digest[self->bh[bi].dindex] != '\0') {
    assert(remain > 0);
    *result = self->bh[bi].digest[self->bh[bi].dindex];
    if((flags & FUZZY_FLAG_ELIMSEQ) == 0 || i < 3 ||
       *result != result[-1] ||
       *result != result[-2] ||
       *result != result[-3]) {
      ++result;
      --remain;
    }
  }
  assert(remain > 0);
  *result++ = ':';
  --remain;
  if (bi < self->bhend - 1)
  {
    ++bi;
    i = (int)self->bh[bi].dindex;
    if ((flags & FUZZY_FLAG_NOTRUNC) == 0 &&
	i > SPAMSUM_LENGTH / 2 - 1)
      i = SPAMSUM_LENGTH / 2 - 1;
    assert(i <= remain);
    if ((flags & FUZZY_FLAG_ELIMSEQ) != 0)
      i = memcpy_eliminate_sequences(result,
				     self->bh[bi].digest, i);
    else
      memcpy(result, self->bh[bi].digest, (size_t)i);
    result += i;
    remain -= i;
    if (h != 0) {
      assert(remain > 0);
      h = (flags & FUZZY_FLAG_NOTRUNC) != 0 ? self->bh[bi].h :
	self->bh[bi].halfh;
      *result = b64[h % 64];
      if ((flags & FUZZY_FLAG_ELIMSEQ) == 0 || i < 3 ||
	  *result != result[-1] ||
	  *result != result[-2] ||
	  *result != result[-3])
      {
	++result;
	--remain;
      }
    } else {
      i = (flags & FUZZY_FLAG_NOTRUNC) != 0 ?
        self->bh[bi].digest[self->bh[bi].dindex] : self->bh[bi].halfdigest;
      if (i != '\0') {
	assert(remain > 0);
	*result = i;
	if ((flags & FUZZY_FLAG_ELIMSEQ) == 0 || i < 3 ||
	    *result != result[-1] ||
	    *result != result[-2] ||
	    *result != result[-3])
	{
	  ++result;
	  --remain;
	}
      }
    }
  } else if (h != 0)
    {
      assert(bi == 0 || bi == NUM_BLOCKHASHES - 1);
      assert(remain > 0);
      if (bi == 0)
	*result++ = b64[self->bh[bi].h % 64];
      else
	*result++ = b64[self->lasth % 64];
      /* No need to bother with FUZZY_FLAG_ELIMSEQ, because this
       * digest has length 1. */
      --remain;
    }
  *result = '\0';
  return 0;
}

void fuzzy_free(/*@only@*/ struct fuzzy_state *self)
{
  free(self);
}

int fuzzy_hash_buf(const unsigned char *buf,
		   uint32_t buf_len,
		   /*@out@*/ char *result)
{
  struct fuzzy_state *ctx;
  int ret = -1;
  if (NULL == (ctx = fuzzy_new()))
    return -1;
  if (fuzzy_set_total_input_length(ctx, buf_len) < 0)
    goto out;
  if (fuzzy_update(ctx, buf, buf_len) < 0)
    goto out;
  if (fuzzy_digest(ctx, result, 0) < 0)
    goto out;
  ret = 0;
 out:
  fuzzy_free(ctx);
  return ret;
}

static int fuzzy_update_stream(struct fuzzy_state *state,
			       FILE *handle)
{
  unsigned char buffer[4096];
  size_t n;
  for(;;)
  {
    n = fread(buffer, 1, 4096, handle);
    if (0 == n)
      break;
    if (fuzzy_update(state, buffer, n) < 0)
      return -1;
  }
  if (ferror(handle) != 0)
    return -1;
  return 0;
}

int fuzzy_hash_stream(FILE *handle, /*@out@*/ char *result)
{
  struct fuzzy_state *ctx;
  int ret = -1;
  if (NULL == (ctx = fuzzy_new()))
    return -1;
  if (fuzzy_update_stream(ctx, handle) < 0)
    goto out;
  if (fuzzy_digest(ctx, result, 0) < 0)
    goto out;
  ret = 0;
 out:
  fuzzy_free(ctx);
  return ret;
}

#ifdef S_SPLINT_S
typedef size_t off_t;
int fseeko(FILE *, off_t, int);
off_t ftello(FILE *);
#endif

int fuzzy_hash_file(FILE *handle, /*@out@*/ char *result)
{
  off_t fpos, fposend;
  int status = -1;
  struct fuzzy_state *ctx;

  fpos = ftello(handle);
  if (fpos < 0)
  {
    printf ("error1\n");
    return -1;
  }
  if (fseeko(handle, 0, SEEK_END) < 0)
  {
    printf ("error2\n");
    return -1;
  }
  fposend = ftello(handle);
  if (fposend < 0)
  {
    printf ("error3\n");
    return -1;
  }
  if (fseeko(handle, 0, SEEK_SET) < 0)
  {
    printf ("error4\n");
    return -1;
  }
  if (NULL == (ctx = fuzzy_new()))
  {
    printf ("error5\n");
    return -1;
  }
  if (fuzzy_set_total_input_length(ctx, (uint_least64_t)fposend) < 0)
  {
    printf ("error5\n");
    goto out;
  }
  if (fuzzy_update_stream(ctx, handle) < 0)
  {
    printf ("error6\n");
    goto out;
  }
  status = fuzzy_digest(ctx, result, 0);
out:
  if (status == 0)
  {
    if (fseeko(handle, fpos, SEEK_SET) < 0)
      return -1;
  }
  fuzzy_free(ctx);
  return status;
}

int fuzzy_hash_filename(const char *filename, /*@out@*/ char *result)
{
  int status;
  FILE *handle = fopen(filename, "rb");
  if (NULL == handle)
    return -1;
  status = fuzzy_hash_stream(handle, result);
  /* We cannot do anything about an fclose failure. */
  (void)fclose(handle);
  return status;
}

//tttt
#define ROLLING_WINDOW1 4

struct roll_state1 {
  unsigned char window[ROLLING_WINDOW1];
  uint32_t h1, h2, h3;
  uint32_t n;
};

static void roll_hash1(struct roll_state1 *self, unsigned char c)
{
  self->h2 -= self->h1;
  self->h2 += ROLLING_WINDOW1 * (uint32_t)c;

  self->h1 += (uint32_t)c;
  self->h1 -= (uint32_t)self->window[self->n];

  self->window[self->n] = c;
  self->n++;
  if (self->n == ROLLING_WINDOW1)
    self->n = 0;

  /* The original spamsum AND'ed this value with 0xFFFFFFFF which
   * in theory should have no effect. This AND has been removed
   * for performance (jk) */
  self->h3 <<= 5;
  self->h3 ^= c;
}

static uint32_t roll_sum1(const struct roll_state1 *self)
{
  return self->h1 + self->h2 + self->h3;
}
static void roll_init1(/*@out@*/ struct roll_state1 *self) {
	memset(self, 0, sizeof(struct roll_state1));
}

// We only accept a match if we have at least one common substring in
// the signature of length ROLLING_WINDOW. This dramatically drops the
// false positive rate for lowcore thresholds while having
// negligable affect on the rate of spam detection.
//
// return 1 if the two strings do have a common substring, 0 otherwise
//
static int has_common_substring(const char *s1, const char *s2)
{
  int i, j;
  int num_hashes;
  uint32_t hashes[SPAMSUM_LENGTH];

  // there are many possible algorithms for common substring
  // detection. In this case I am re-using the rolling hash code
  // to act as a filter for possible substring matches

  memset(hashes, 0, sizeof(hashes));

  // first compute the windowed rolling hash at each offset in
  // the first string
  struct roll_state1 state;
  roll_init1 (&state);

  for (i=0;s1[i];i++)
  {
    roll_hash1(&state, (unsigned char)s1[i]);
    hashes[i] = roll_sum1(&state);
  }
  num_hashes = i;

  roll_init1(&state);

  // now for each offset in the second string compute the
  // rolling hash and compare it to all of the rolling hashes
  // for the first string. If one matches then we have a
  // candidate substring match. We then confirm that match with
  // a direct string comparison */
  for (i=0;s2[i];i++)
  {
    roll_hash1(&state, (unsigned char)s2[i]);
    uint32_t h = roll_sum1(&state);
    if (i < ROLLING_WINDOW1-1) continue;
    for (j=ROLLING_WINDOW1-1;j<num_hashes;j++)
    {
      if (hashes[j] == h)
      {
	// we have a potential match - confirm it
	if (strlen(s2+i-(ROLLING_WINDOW1-1)) >= ROLLING_WINDOW1 &&
	    strncmp(s2+i-(ROLLING_WINDOW1-1),
		    s1+j-(ROLLING_WINDOW1-1),
		    ROLLING_WINDOW1) == 0)
	{
	  return 1;
	}
      }
    }
  }

  return 0;
}


// eliminate sequences of longer than 3 identical characters. These
// sequences contain very little information so they tend to just bias
// the result unfairly
static char *eliminate_sequences(const char *str)
{
  char *ret;
  size_t i, j, len;

  ret = strdup(str);
  if (!ret)
    return NULL;

  len = strlen(str);
  if (len < 3)
    return ret;

  for (i=j=3 ; i<len ; i++)
  {
    if (str[i] != str[i-1] ||
	str[i] != str[i-2] ||
	str[i] != str[i-3])
    {
      ret[j++] = str[i];
    }
  }

  ret[j] = 0;

  return ret;
}

//
// this is the low level string scoring algorithm. It takes two strings
// and scores them on a scale of 0-100 where 0 is a terrible match and
// 100 is a great match. The block_size is used to cope with very small
// messages.
//
static uint32_t score_strings(const char *s1,
			      const char *s2,
			      unsigned long block_size)
{
  uint32_t score;
  size_t len1, len2;

  len1 = strlen(s1);
  len2 = strlen(s2);

  if (len1 > SPAMSUM_LENGTH || len2 > SPAMSUM_LENGTH) {
    // not a real spamsum signature?
    printf ("asdf");
    return 0;
  }

  // the two strings must have a common substring of length
  // ROLLING_WINDOW to be candidates

  if (has_common_substring(s1, s2) == 0) {
    printf ("dsddd");
    return 0;
  }
  else{
      return 1;
  }

  // compute the edit distance between the two strings. The edit distance gives
  // us a pretty good idea of how closely related the two strings are
  score = edit_distn(s1, len1, s2, len2);


  // scale the edit distance by the lengths of the two
  // strings. This changes the score to be a measure of the
  // proportion of the message that has changed rather than an
  // absolute quantity. It also copes with the variability of
  // the string lengths.
  score = (score * SPAMSUM_LENGTH) / (len1 + len2);

  // at this stage the score occurs roughly on a 0-SPAMSUM_LENGTH scale,
  // with 0 being a good match and SPAMSUM_LENGTH being a complete
  // mismatch

  // rescale to a 0-100 scale (friendlier to humans)
  score = (100 * score) / SPAMSUM_LENGTH;

  // now re-scale on a 0-100 scale with 0 being a poor match and
  // 100 being a excellent match.
  score = 100 - score;

  //  printf ("len1: %"PRIu32"  len2: %"PRIu32"\n", len1, len2);

  // when the blocksize is small we don't want to exaggerate the match size
  if (block_size >= (99 + ROLLING_WINDOW) / ROLLING_WINDOW * MIN_BLOCKSIZE)
    return score;
  if (score > block_size/MIN_BLOCKSIZE * MIN(len1, len2))
  {
    score = block_size/MIN_BLOCKSIZE * MIN(len1, len2);
  }
  return score;
}

//
// Given two spamsum strings return a value indicating the degree
// to which they match.
//
int fuzzy_compare(const char *str1, const char *str2)
{
  unsigned long block_size1, block_size2;
  uint32_t score = 0;
  char *s1, *s2;
  char *s1_1, *s1_2, *s1_3;
  char *s2_1, *s2_2, *s2_3;

  if (NULL == str1 || NULL == str2)
    return -1;

  // each spamsum is prefixed by its block size
  if (sscanf(str1, "%lu:", &block_size1) != 1 ||
      sscanf(str2, "%lu:", &block_size2) != 1) {
    return -1;
  }

  // if the blocksizes don't match then we are comparing
  // apples to oranges. This isn't an 'error' per se. We could
  // have two valid signatures, but they can't be compared.
  if (block_size1 != block_size2 &&
      (block_size1 > ULONG_MAX / 2 || block_size1*2 != block_size2) &&
      (block_size1 % 2 == 1 || block_size1 / 2 != block_size2)) {
      printf ("different block size");
    return 100;
  }

  // move past the prefix
  str1 = strchr(str1, ':');
  str2 = strchr(str2, ':');

  if (!str1 || !str2) {
    // badly formed ...
    return -1;
  }

  // there is very little information content is sequences of
  // the same character like 'LLLLL'. Eliminate any sequences
  // longer than 3. This is especially important when combined
  // with the has_common_substring() test below.
  // NOTE: This function duplciates str1 and str2
  s1 = eliminate_sequences(str1+1);
  if (!s1)
  {
    printf ("1null");
    return 100;
  }
  s2 = eliminate_sequences(str2+1);
  if (!s2)
  {
    printf ("2null");
    free(s1);
    return 100;
  }

  // now break them into the two pieces
  s1_1 = s1;
  s2_1 = s2;

  s1_2 = strchr(s1, ':');
  s2_2 = strchr(s2, ':');

  if (!s1_2 || !s2_2) {
    // a signature is malformed - it doesn't have 2 parts
    free(s1); free(s2);
    return -1;
  }

  // Advance the pointer to the start of the second substring.
  s1_2++;
  s2_2++;

  // Chop the second string at the comma--just before the filename.
  // If the strings don't have a comma (i.e. don't have a filename)
  // that's ok. It's not an error. This function can be called on
  // signatures which don't have filenames attached.
  // We also don't have to advance past the comma however. We don't care
  // about the filename
  s1_3 = strchr(s1_2, ',');
  s2_3 = strchr(s2_2, ',');

  s1_3++;
  s2_3++;

  char *s1_4, *s2_4;
  s1_4 = strchr(s1_3, ',');
  s2_4 = strchr(s2_3, ',');

  s1_4++;
  s2_4++;


  int a, b, c, d;

  if (s1_3[1] == '\0')
  {
      a = s1_3[1] - 48;
  }
  else
  {
  a = (s1_3[0]-48) * 10 + s1_3[1] - 48;
  }

  if (s1_3[1] == '\0')
  {
      b = s2_3[1] - 48;
  }
  else
  {
      b = (s2_3[0]-48) * 10 + s2_3[1] - 48;
  }
  if (s1_4[1] == '\0')
  {
      c = s1_4[1] - 48;
  }
  else
  {
      c = (s1_4[0]-48) * 10 + s1_4[1] - 48;
  }
  if (s2_4[1] == '\0')
  {
      d = s2_4[1] - 48;
  }
  else
  {
      d = (s1_4[0]-48) * 10 + s2_4[1] - 48;
  }

  score = area_dist(a,b,c,d);

  if (s1_3 != NULL)
    *s1_3 = 0;
  if (s2_3 != NULL)
    *s2_3 = 0;

  // Now that we know the strings are both well formed, are they
  // identical? We could save ourselves some work here
  if (block_size1 == block_size2 && strlen(s1) == strlen(s2)) {
    if (!strncmp(s1, s2, strlen(s1))) {
      free (s1);
      free (s2);
      return 100;
    }
  }

  // Chop the first substring.
  s1_2[-1] = 0;
  s2_2[-1] = 0;

  int k;
  // each signature has a string for two block sizes. We now
  // choose how to combine the two block sizes. We checked above
  // that they have at least one block size in common
  if (block_size1 <= ULONG_MAX / 2) {
    if (block_size1 == block_size2) {
      uint32_t score1, score2;
      score1 = score_strings(s1_1, s2_1, block_size1);
      score2 = score_strings(s1_2, s2_2, block_size1*2);
      k = MAX(score1, score2);
    }
    else if (block_size1 * 2 == block_size2) {
      k = score_strings(s1_2, s2_1, block_size2);
    }
    else {
      k = score_strings(s1_1, s2_2, block_size1);
    }
  }
  else {
    if (block_size1 == block_size2) {
      k = score_strings(s1_1, s2_1, block_size1);
    }
    else if (block_size1 % 2 == 0 && block_size1 / 2 == block_size2) {
      k = score_strings(s1_1, s2_2, block_size1);
    }
    else {
      printf("blocksizediff");
      score = 100;
    }
  }

  if (k != 0)
  {
      return score;
  }
  else
  {
      printf ("k-0\n");
      return 100;
  }

  free(s1);
  free(s2);

  return (int)score;
}

int cmptimes(int index)
{
    int  j, compare;
    char filename1[10000], filename2[10000];
    char str1[10000], str2[10000];
    char stindex[5] = "";
    FILE *fp1, *fp2;
    char res1[10000];
    char res2[10000];
    int add1, add2;


    filename1[0] = '\0';
    filename2[0] = '\0';
    stindex[5]='0';
    memset(res1, '\0', 10000);
    memset(res2, '\0', 10000);
    fp1 = NULL;
    fp2 = NULL;
    if (index<10)
    {
        stindex[0] = index+48;
    }
    else
    {
        sprintf(stindex, "%d", index);
    }

    strcat(filename1, "../tests/plaintext/farplaintext_");
    strcat(filename1, stindex);

    strcat(filename2, filename1);

    strcat(filename1, "-1.txt");
    strcat(filename2, "-2.txt");

    fp1 = fopen(filename1, "r");
    fuzzy_hash_file(fp1, res1);

    fgets(str1, 1000, fp1);
    add1 = area_distx(str1);
    add2 = area_disty(str1);
    printf ("str1: %s, add: %d, add2: %d\n", str1, add1, add2);
    sprintf(stindex, "%d", add1);
    strcat(res1, ",");
    strcat(res1, stindex);
    strcat(res1, ",");
    sprintf(stindex, "%d", add2);
    strcat(res1, stindex);
    printf("%s\n", res1);


    fp2 = fopen(filename2, "r");
    fuzzy_hash_file(fp2, res2);

    fgets(str2, 1000, fp2);
    add1 = area_distx(str2);
    add2 = area_disty(str2);
    printf ("str2: %s\n", str2);
    sprintf(stindex, "%d", add1);
    strcat(res2, ",");
    strcat(res2, stindex);
    strcat(res2, ",");
    sprintf(stindex, "%d", add2);
    strcat(res2, stindex);
    printf("%s\n", res2);

    compare = fuzzy_compare(res1, res2);

    printf ("compare=%d\n", compare);

    fclose(fp2);
    fclose(fp1);

    return compare;

}

int main(void){

    char *str1;
    char *str2;
    char *str3;
    char *str4;
    int result, i, sum, count, loop;
    char res[] = "";

    sum = 0;
    count = 0;
    loop = 10000;
    for (i=0 ; i < loop ; i++)
    {
        result = cmptimes(i);
        sum = sum + result;
        if (result < 5)
        {
            count++;
        }
    }

 //   str1 = "6757475645343322323130";
 //   printf ("%d\n", area_distx(str1));

   printf ("\n\n\n\ncount: %d sum: %d, avgc: %lf, avgsum: %lf, avg>0: %lf\n", count, sum, count/(float)loop, sum/(float)loop, sum/(float)count);

    return 0;

}
