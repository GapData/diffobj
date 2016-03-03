/* This an adaptation of the Michael B. Allen implementation of the
 * Myers diff algorithm (see below for details)
 *
 * The main changes are adapting to use in an R context (memory allocations with
 * R_alloc) and simplifying code by removing the variable arrays and the
 * ability to specify custom comparison functions.  Because we don't use the
 * variable arrays we need to pre-allocate 4 * (n + m + abs(n - m)) + 1 vector
 * which is wasteful but still linear so should be okay.
 */

/* diff - compute a shortest edit script (SES) given two sequences
 * Copyright (c) 2004 Michael B. Allen <mba2000 ioplex.com>
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/* This algorithm is basically Myers' solution to SES/LCS with
 * the Hirschberg linear space refinement as described in the
 * following publication:
 *
 *   E. Myers, ``An O(ND) Difference Algorithm and Its Variations,''
 *   Algorithmica 1, 2 (1986), 251-266.
 *   http://www.cs.arizona.edu/people/gene/PAPERS/diff.ps
 *
 * This is the same algorithm used by GNU diff(1).
 */

#include <stdlib.h>
#include "diffobj.h"

#define FV(k) _v(ctx, (k), 0)
#define RV(k) _v(ctx, (k), 1)
/* we've abandonned the use of varray for both ses and buf, instead we
 * pre-allocate both the edit script and the buffer to the maximum possible size
 */
struct _ctx {
  void *context;
  int *buf;               // used to be varray
  int bufmax;
  struct diff_edit *ses;  // used to be varray
  int si;
  int simax;
  int dmax;
};

struct middle_snake {
  int x, y, u, v;
};
/* debugging util */
const char * _op_to_chr(diff_op op) {
    switch (op) {
      case DIFF_MATCH: return "match"; break;
      case DIFF_DELETE: return "delete"; break;
      case DIFF_INSERT: return "insert"; break;
      case DIFF_NULL: return "NULL"; break;
      default:
        error("Logic Error: unexpected faux snake instruction; contact maintainer");
    }
}
/*
 * k = diagonal number
 * val = x value
 * r = presumably whether we are looking up in reverse snakes
 */
  static void
_setv(struct _ctx *ctx, int k, int r, int val)
{
  int j;
  int *i;
  /* Pack -N to N into 0 to N * 2, but also pack reverse and forward snakes
   * in that same space which is why we need the * 4
   */
  j = k <= 0 ? -k * 4 + r : k * 4 + (r - 2);

  if(j > ctx->bufmax || j < 0) {
    error(
      "Logic Error: exceeded buffer size (%d vs %d); contact maintainer.",
      j, ctx->bufmax
    );
  }
  i = ctx->buf + j;
  *i = val;
}
/*
 * For any given `k` diagonal, return the x coordinate of the furthest reaching
 * path we've found.  Use `r` to look for the x coordinate for the paths that
 * are starting from the bottom right instead of top left
 */
  static int
_v(struct _ctx *ctx, int k, int r)
{
  int j;

  j = k <= 0 ? -k * 4 + r : k * 4 + (r - 2);
  if(j > ctx->bufmax || j < 0)
    error(
      "Logic Error: exceeded buffer 2 size (%d vs %d); contact maintainer.",
      j, ctx->bufmax
    );

  int bufval = *(ctx->buf + j);
  return bufval;
}
/* Compare character vector values
 *
 * Needs to account for special case when indeces are oob for the strings.  The
 * oob checks seem to be necessary since algo is requesting reads past length
 * of string, but not sure if that is intentional or not.  In theory it should
 * not be, but perhaps this was handled gracefully by the varray business b4
 * we changed it.
 */
int _comp_chr(SEXP a, int aidx, SEXP b, int bidx) {
  int alen = XLENGTH(a);
  int blen = XLENGTH(b);
  int comp;
  if(aidx >= alen && bidx >= blen) {
    comp = 1;
  } else if(aidx >= alen || bidx >= blen) {
    comp = 0;
  } else comp = STRING_ELT(a, aidx) == STRING_ELT(b, bidx);
  return(comp);
}
/*
 * Handle cases where differences exceed maximum allowable differences
 *
 * General logic is to create a faux snake that instead of moving down
 * diagonally will chain right and down moves until it hits a path coming
 * from the other direction.  This snake is stored in `ctx`, and is then
 * written by `ses` to the `ses` list.
 */
static int
_find_faux_snake(
  SEXP a, int aoff, int n, SEXP b, int boff, int m, struct _ctx *ctx,
  struct middle_snake *ms, int d, diff_op ** faux_snake
) {
  /* normally we would record k/x values at the end of the furthest reaching
   * snake, but here we need pick a path from top left  and extend it until
   * we hit something coming from bottom right.
   */
  /* start by finding which diagonal has the furthest reaching value
   * when looking from top left
   */
  int k_max_f = 0, x_max_f = -1;
  int x_f, y_f, k_f;
  int delta = n - m;

  for (int k = d - 1; k >= -d + 1; k -= 2) { /* might need to shift by 1 */
    int x_f = FV(k);
    int f_dist = x_f - abs(k);

    if(f_dist > x_max_f - abs(k_max_f)) {
      x_max_f = x_f;
      k_max_f = k;
    }
  }
  /* didn't find a path so use origin */
  if(x_max_f < 0) {
    x_f = y_f = k_f = 0;
  } else {
    k_f = k_max_f;
    x_f = x_max_f;
    y_f = x_f - k_max_f;
  }
  /*
   * now look for the furthest reaching point in any diagonal that is
   * below the diagonal we found above since those are the only ones we
   * can connect to
   *
   * Watch out `k_f` and `k_r` are not in direct correspondance as they must
   * be shifted by `delta`
   */
  int k_max_r = 0, x_max_r = n + 1;
  int x_r, y_r, k_r;

  for (int k = -d; k <= k_max_f - delta; k += 2) {
    int x_r = RV(k);
    int r_dist = n - x_r - abs(k);

    /* since buffer is init to zero, an x_r value of zero means nothing, and
     * not all the way to the left of the graph; also, in reverse snakes the
     * snake should end at x == 1 in the leftmost case (we think)
     */
    if(r_dist > n - x_max_r - abs(k_max_r) && x_r) {
      x_max_r = x_r;
      k_max_r = k;
    }
  }
  /* didn't find a path so use origin */
  if(x_max_r > n) {
    x_r = n; y_r = m; k_r = 0;
  } else {
    k_r = k_max_r;
    x_r = x_max_r;
    y_r = x_r - k_max_r;
  }
  /*
   * attempt to connect the two paths we found.  We need to store this
   * information as our "faux" snake since it will have to be processed
   * in a manner similar as the middle snake would be processed; start by
   * figuring out max number of steps it would take to connect the two
   * paths
   */
  int max_steps = x_r - x_f + y_r - y_f + 1;
  int steps = 0;
  int step_dir = 1; /* last direction we moved in, 1 is down */
  int x_sn = x_f, y_sn = y_f;

  /* initialize the fake snake */
  if(max_steps < 0) error("Logic Error: fake snake step overflow? Contact maintainer.");

  Rprintf("allocate faux snake\n");
  diff_op * faux_snake_tmp = (diff_op*) R_alloc(max_steps, sizeof(diff_op));
  for(int i = 0; i < max_steps; i++) *(faux_snake_tmp + i) = DIFF_NULL;
  Rprintf("done allocating faux snake\n");

  while(x_sn < x_r || y_sn < y_r) {
    if(x_sn > x_r || y_sn > y_r) {
      error("Logic Error: Exceeded buffer for finding fake snake; contact maintainer.");
    }
    /* check to see if we could possibly move on a diagonal, and do so
     * if possible, if not alternate going down and right*/
    if(
        x_sn <= x_r && y_sn <= y_r &&
        _comp_chr(a, aoff + x_sn, b, boff + y_sn)
    ) {
      x_sn++; y_sn++;
      *(faux_snake_tmp + steps) = DIFF_MATCH;
    } else if (x_sn < x_r && (step_dir || y_sn >= y_r)) {
      x_sn++;
      step_dir = !step_dir;
      *(faux_snake_tmp + steps) = DIFF_DELETE;
    } else if (y_sn < y_r && (!step_dir || x_sn >= x_r)) {
      y_sn++;
      *(faux_snake_tmp + steps) = DIFF_INSERT;
      step_dir = !step_dir;
    } else {
      Rprintf(
        "x_sn: %d y_sn: %d x_r: %d, y_r: %d steps: %d max_steps: %d\n",
        x_sn, y_sn, x_r, y_r, steps, max_steps
      );
      error("Logic Error: unexpected outcome in snake creation process; contact maintainer");
    }
    steps++;
  }
  /* corner cases; must absolutely make sure steps LT max_steps since we rely
   * on at least one zero at the end of the faux_snake when we read it to know
   * to stop reading it
   */
  if(x_sn != x_r || y_sn != y_r || steps >= max_steps) {
    Rprintf(
      "x_sn: %d y_sn: %d x_r: %d, y_r: %d steps: %d max_steps: %d\n",
      x_sn, y_sn, x_r, y_r, steps, max_steps
    );
    error("Logic Error: faux snake process failed; contact maintainer.");
  }
  /* modify the pointer to the pointer so we can return in by ref */

  *faux_snake = faux_snake_tmp;

  /* record the coordinates of our faux snake using `ms` */
  ms->x = x_f;
  ms->y = y_f;
  ms->u = x_r;
  ms->v = y_r;

  return d;
}

/*
 * Advance from both ends of the diff graph toward center until we reach
 * up to half of the maximum possible number of differences between
 * a and b (note that `n` is net of `aoff`).  As we process this we record the
 * end points of each path we explored in the `ctx` structure.  Once we reach
 * the maximum number of differences, we return to `_ses` with the number of
 * differences found.  `_ses` will then attempt to stitch back the snakes
 * together.
 */
  static int
_find_middle_snake(
  SEXP a, int aoff, int n, SEXP b, int boff, int m, struct _ctx *ctx,
  struct middle_snake *ms, diff_op ** faux_snake
) {
  int delta, odd, mid, d;

  delta = n - m;
  odd = delta & 1;
  mid = (n + m) / 2;
  mid += odd;

  _setv(ctx, 1, 0, 0);
  _setv(ctx, delta - 1, 1, n);

  /* For each number of differences `d`, compute the farthest reaching paths
   * from both the top left and bottom right of the edit graph
   */
  for (d = 0; d <= mid; d++) {
    int k, x, y;

    /* reached maximum allowable differences before real exit condition*/
    if ((2 * d - 1) >= ctx->dmax) {
      return _find_faux_snake(a, aoff, n, b, boff, m, ctx, ms, d, faux_snake);
    }
    /* Forward (from top left) paths*/

    for (k = d; k >= -d; k -= 2) {
      if (k == -d || (k != d && FV(k - 1) < FV(k + 1))) {
        x = FV(k + 1);
      } else {
        x = FV(k - 1) + 1;
      }
      y = x - k;

      ms->x = x;
      ms->y = y;
      while(x < n && y < m && _comp_chr(a, aoff + x, b, boff + y)) {
        /* matching characters, just walk down diagonal */
        x++; y++;
      }
      _setv(ctx, k, 0, x);

      /* for this diagonal we (think we) are now at farthest reaching point for
       * a given d.  Then return if:
       * - If we're at the edge of the addressable part of the graph
       * - The reverse snakes are already overlapping in the `x` coordinate
       *
       * then it means that the only way to get to the snake coming from the
       * other direction is by either moving down or across for every remaining
       * move, so record the current coord as `u` and `v` and return
       *
       * Note that for the backward snake we reverse xy and uv so that the
       * matching snake is always defined in `ms` as starting at `ms.(xy)` and
       * ending at `ms.(uv)`
       */
      if (odd && k >= (delta - (d - 1)) && k <= (delta + (d - 1))) {
        if (x >= RV(k)) {
          ms->u = x;
          ms->v = y;
          return 2 * d - 1;
        }
      }
    }
    /* Backwards (from bottom right) paths*/

    for (k = d; k >= -d; k -= 2) {
      int kr = (n - m) + k;

      if (k == d || (k != -d && RV(kr - 1) < RV(kr + 1))) {
        x = RV(kr - 1);
      } else {
        x = RV(kr + 1) - 1;
      }
      y = x - kr;

      ms->u = x;
      ms->v = y;

      while (x > 0 && y > 0 && _comp_chr(a, aoff + x - 1, b, boff + y - 1)) {
        /* matching characters, just walk up diagonal */
        x--; y--;
      }
      _setv(ctx, kr, 1, x);

      /* see comments in forward section */
      if (!odd && kr >= -d && kr <= d) {
        if (x <= FV(kr)) {
          ms->x = x;
          ms->y = y;
          return 2 * d;
        }
      }
    }
  }
  error("Logic Error: failed finding middle snake, contact maintainer");
}
/*
 * Update edit script atom with newest info, we record the operation, and the
 * offset and length so we can recover the values from the original vector
 */
  static void
_edit(struct _ctx *ctx, int op, int off, int len)
{
  struct diff_edit *e;

  if (len == 0 || ctx->ses == NULL) {
    return;
  }               /* Add an edit to the SES (or
                   * coalesce if the op is the same)
                   */
  e = ctx->ses + ctx->si;
  if(ctx->si > ctx->simax)
    error("Logic Error: exceed edit script length; contact maintainer.");
  if (e->op != op) {
    if (e->op) {
      ctx->si++;
      if(ctx->si > ctx->simax)
        error("Logic Error: exceed edit script length; contact maintainer.");
      e = ctx->ses + ctx->si;
    }
    e->op = op;
    e->off = off;
    e->len = len;
  } else {
    e->len += len;
  }
}
/*
 * Update edit script with the faux diff data
 */
  static void
_edit_faux(struct _ctx *ctx, diff_op * faux_snake, int aoff, int boff) {
  int i = 0, off;
  diff_op op;
  while((op = *(faux_snake + i++)) != DIFF_NULL) {
    switch (op) {
      case DIFF_MATCH: boff++;  /* note no break here */
      case DIFF_DELETE: off = aoff++;
        break;
      case DIFF_INSERT: off = boff++;
        break;
      default:
        error("Logic Error: unexpected faux snake instruction; contact maintainer");
    }
    /* use x (aoff) offset for MATCH and DELETE, y offset for INSERT */
    _edit(ctx, op, off, 1);
  }
}
/* Generate shortest edit script
 *
 */
  static int
_ses(
  SEXP a, int aoff, int n, SEXP b, int boff, int m, struct _ctx *ctx
) {
  R_CheckUserInterrupt();
  struct middle_snake ms;
  int d;

  if (n == 0) {
    _edit(ctx, DIFF_INSERT, boff, m);
    d = m;
  } else if (m == 0) {
    _edit(ctx, DIFF_DELETE, aoff, n);
    d = n;
  } else {
    /* Find the middle "snake" around which we
     * recursively solve the sub-problems.  Note this modifies `ms` by ref to
     * set the beginning and end coordinates of the snake of the furthest
     * reaching path.  The beginning is always the top left part of the snake,
     * irrespective of whether it was found on a forward or reverse path as
     * f_m_s will flip the coordinates when appropriately when recording them
     * in `ms`
     *
     * Additionally, if diffs exceed max.diffs, then `faux.snake` will also
     * be set.  `faux_snake` is a pointer to a pointer that points to a the
     * beginning of an array of match/delete/insert instructions generated
     * to connect the top left and bottom right paths. _fms() repoints the
     * pointer to an updated edit list if needed via (_ffs())
     */
    diff_op fsv = DIFF_NULL;
    diff_op * faux_snake;
    faux_snake = &fsv;
    //
    // d
    // diff_op * fsp = NULL;
    // diff_op fsv = DIFF_NULL;
    // *fsp = fsv;
    // **faux_snake = *fsp;

    Rprintf("sn1: %s\n", _op_to_chr(*faux_snake));
    d = _find_middle_snake(a, aoff, n, b, boff, m, ctx, &ms, &faux_snake);
    Rprintf("sn2: %s\n", _op_to_chr(*faux_snake));
    if(*faux_snake) Rprintf("Snake is fake!\n");
    if (d == -1) {
      error(
        "Logic error: failed trying to find middle snake, contact maintainer."
      );
    } else if (d >= ctx->dmax) {
      return ctx->dmax;
    } else if (ctx->ses == NULL) {
      return d;
    } else if (d > 1) {
      /* in this case we have something along the lines of (note the non-
       * diagonal bits are just non-diagonal, we're making no claims about
       * whether they should or could be of the horizontal variety)
       * ... -
       *      \
       *       \
       *        \- ...
       * so we will record the snake (diagonal) in the middle, and recurse
       * on the stub at the beginning and on the stub at the end separately
       */

      /* Beginning stub */

      if (_ses(a, aoff, ms.x, b, boff, ms.y, ctx) == -1) {
        error("Logic error: failed trying to run ses; contact maintainer.");
      }
      /* Now record middle snake
       *
       * u should be x coord of end of snake of longest path
       * v should be y coord of end of snake
       * x, y should be coord of begining of snake
       *
       * record that there is a matching section between the beginning of the
       * middle snake and the end
       *
       * if faux_snake is defined it means that there were too many differences
       * to complete algorigthm normally so we need to record the faux snake
       */
      if(*faux_snake) {
        /* for faux snake length of snake will most likely not be ms.u - ms.x
         * since it will not be a diagonal
         */
        Rprintf("Faux edit at offs: %d %d\n", ms.x, ms.y);
        _edit_faux(ctx, faux_snake, aoff + ms.x, boff + ms.y);
      } else {
        _edit(ctx, DIFF_MATCH, aoff + ms.x, ms.u - ms.x);
      }
      /* Now recurse into the second stub */
      aoff += ms.u;
      boff += ms.v;
      n -= ms.u;
      m -= ms.v;
      if (_ses(a, aoff, n, b, boff, m, ctx) == -1) {
        error("Logic error: failed trying to run ses 2; contact maintainer.");
      }
    } else {
      int x = ms.x;
      int u = ms.u;

      /* There are only 4 base cases when the
       * edit distance is 1.
       *
       * n > m   m > n
       *
       *   -       |
       *    \       \    x != u
       *     \       \
       *
       *   \       \
       *    \       \    x == u
       *     -       |
       */

      if (m > n) {
        if (x == u) {
          _edit(ctx, DIFF_MATCH, aoff, n);
          _edit(ctx, DIFF_INSERT, boff + (m - 1), 1);
        } else {
          _edit(ctx, DIFF_INSERT, boff, 1);
          _edit(ctx, DIFF_MATCH, aoff, n);
        }
      } else if (m < n) {
        if (x == u) {
          _edit(ctx, DIFF_MATCH, aoff, m);
          _edit(ctx, DIFF_DELETE, aoff + (n - 1), 1);
        } else {
          _edit(ctx, DIFF_DELETE, aoff, 1);
          _edit(ctx, DIFF_MATCH, aoff + 1, m);
        }
      } else {
        // Should never get here since this should be a D 2 case
        error("Very special case n %d m %d aoff %d boff %d u %d\n", n, m, aoff, boff, ms.u);
      }
    }
  }
  return d;
}
/*
 * - ses is a pointer to an array of diff_edit structs that is initialized
 *   outside of this call
 * - aoff and boff are how far we've moved across the strings, used mostly in the
 *   context of recursion for _ses
 * - n is the lenght of a, m the length of b
 */
  int
diff(SEXP a, int aoff, int n, SEXP b, int boff, int m,
  void *context, int dmax, struct diff_edit *ses, int *sn
) {
  if(n < 0 || m < 0)
    error("Logic Error: negative lengths; contact maintainer.");
  struct _ctx ctx;
  int d, x, y;
  struct diff_edit *e = NULL;
  int delta = n - m;
  if(delta < 0) delta = -delta;
  int bufmax = 4 * (n + m + delta) + 1;  // see _setv
  if(bufmax < n || bufmax < m)
    error("Logic Error: exceeded maximum allowable combined string length.");

  int *tmp = (int *) R_alloc(bufmax, sizeof(int));
  for(int i = 0; i < bufmax; i++) *(tmp + i) = 0;

  ctx.context = context;

  /* initialize buffer
   */
  ctx.buf = tmp;
  ctx.bufmax = bufmax;
  ctx.ses = ses;
  ctx.si = 0;
  ctx.simax = n + m;
  ctx.dmax = dmax ? dmax : INT_MAX;

  /* initialize first ses edit struct*/
  if (ses && sn) {
    if ((e = ses) == NULL) {
      error("Logic Error: specifying sn, but ses is NULL, contact maintainer.");
    }
    e->op = 0;
  }

  /* The _ses function assumes the SES will begin or end with a delete
   * or insert. The following will ensure this is true by eating any
   * beginning matches. This is also a quick to process sequences
   * that match entirely.
   */
  x = y = 0;
  while (
    x < n && y < m && _comp_chr(a, aoff + x, b, boff + y)
  ) {
    x++; y++;
    if(boff + y < boff + y - 1 || aoff + x < aoff + x - 1)
      error("Logic Error: exceeded int size 45823; contact maintainer");
  }
  _edit(&ctx, DIFF_MATCH, aoff, x);

  d = _ses(a, aoff + x, n - x, b, boff + y, m - y, &ctx);
  if (ses && sn) {
    *sn = e->op ? ctx.si + 1 : 0;
  }
  return d;
}

