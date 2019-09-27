/*****************************************************************************
 *
 * tnumber_gist.c
 *	  R-tree GiST index for temporal integers and temporal floats
 *
 * These functions are based on those in the file gistproc.c.
 * Portions Copyright (c) 2019, Esteban Zimanyi, Arthur Lesuisse, 
 * 		Universite Libre de Bruxelles
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *****************************************************************************/

#include "tnumber_gist.h"

#include <math.h>
#include <access/gist.h>
#include <utils/builtins.h>

#if MOBDB_PGSQL_VERSION >= 120
#include <utils/float.h>
#endif

#include "oidcache.h"
#include "temporal_boxops.h"
#include "temporal_posops.h"

/* Minimum accepted ratio of split */
#define LIMIT_RATIO 0.3

/* Convenience macros for NaN-aware comparisons */
#define FLOAT8_EQ(a,b)	(float8_cmp_internal(a, b) == 0)
#define FLOAT8_LT(a,b)	(float8_cmp_internal(a, b) < 0)
#define FLOAT8_LE(a,b)	(float8_cmp_internal(a, b) <= 0)
#define FLOAT8_GT(a,b)	(float8_cmp_internal(a, b) > 0)
#define FLOAT8_GE(a,b)	(float8_cmp_internal(a, b) >= 0)
#define FLOAT8_MAX(a,b)  (FLOAT8_GT(a, b) ? (a) : (b))
#define FLOAT8_MIN(a,b)  (FLOAT8_LT(a, b) ? (a) : (b))

/*****************************************************************************
 * Static methods
 *****************************************************************************/

/*
 * Calculates union of two tboxes, a and b. The result is stored in *n.
 */
static void
rt_tbox_union(TBOX *n, const TBOX *a, const TBOX *b)
{
	n->xmax = FLOAT8_MAX(a->xmax, b->xmax);
	n->tmax = FLOAT8_MAX(a->tmax, b->tmax);
	n->xmin = FLOAT8_MIN(a->xmin, b->xmin);
	n->tmin = FLOAT8_MIN(a->tmin, b->tmin);
}

/*
 * Size of a TBOX for penalty-calculation purposes.
 * The result can be +Infinity, but not NaN.
 */
static double
size_tbox(const TBOX *box)
{
	/*
	 * Check for zero-width cases.  Note that we define the size of a zero-
	 * by-infinity box as zero.  It's important to special-case this somehow,
	 * as naively multiplying infinity by zero will produce NaN.
	 *
	 * The less-than cases should not happen, but if they do, say "zero".
	 */
	if (FLOAT8_LE(box->xmax, box->xmin) ||
		FLOAT8_LE(box->tmax, box->tmin))
		return 0.0;

	/*
	 * We treat NaN as larger than +Infinity, so any distance involving a NaN
	 * and a non-NaN is infinite.  Note the previous check eliminated the
	 * possibility that the low fields are NaNs.
	 */
	if (isnan(box->xmax))
		return get_float8_infinity();
	return (box->xmax - box->xmin) * (box->tmax - box->tmin);
}

/*
 * Return amount by which the union of the two boxes is larger than
 * the original TBOX's area.  The result can be +Infinity, but not NaN.
 */
static double
box_penalty(const TBOX *original, const TBOX *new)
{
	TBOX unionbox;

	memset(&unionbox, 0, sizeof(TBOX));
	rt_tbox_union(&unionbox, original, new);
	return size_tbox(&unionbox) - size_tbox(original);
}

/*
 * Increase TBOX b to include addon.
 */
static void
adjustBox(TBOX *b, const TBOX *addon)
{
	if (FLOAT8_LT(b->xmax, addon->xmax))
		b->xmax = addon->xmax;
	if (FLOAT8_GT(b->xmin, addon->xmin))
		b->xmin = addon->xmin;
	if (FLOAT8_LT(b->tmax, addon->tmax))
		b->tmax = addon->tmax;
	if (FLOAT8_GT(b->tmin, addon->tmin))
		b->tmin = addon->tmin;
}

/*****************************************************************************
 * The GiST Union method for tboxes
 * Returns the minimal bounding box that encloses all the entries in entryvec
 *****************************************************************************/

PG_FUNCTION_INFO_V1(gist_tbox_union);

Datum
gist_tbox_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int *sizep = (int *) PG_GETARG_POINTER(1);
	int numranges, i;
	TBOX *cur, *pageunion;
	numranges = entryvec->n;
	pageunion = (TBOX *) palloc0(sizeof(TBOX));
	cur = DatumGetTboxP(entryvec->vector[0].key);
	memcpy((void *) pageunion, (void *) cur, sizeof(TBOX));
	for (i = 1; i < numranges; i++)
	{
		cur = DatumGetTboxP(entryvec->vector[i].key);
		adjustBox(pageunion, cur);
	}
	*sizep = sizeof(TBOX);
	PG_RETURN_POINTER(pageunion);
}

/*****************************************************************************
 * The GiST Penalty method for tboxes.
 * As in the R-tree paper, we use change in area as our penalty metric
 *****************************************************************************/

PG_FUNCTION_INFO_V1(gist_tbox_penalty);

Datum
gist_tbox_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origentry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newentry = (GISTENTRY *) PG_GETARG_POINTER(1);
	float *result = (float *) PG_GETARG_POINTER(2);
	TBOX *origbox = DatumGetTboxP(origentry->key);
	TBOX *newbox = DatumGetTboxP(newentry->key);

	*result = (float) box_penalty(origbox, newbox);
	PG_RETURN_POINTER(result);
}

/*****************************************************************************
 * The GiST Split method for tboxes
 *****************************************************************************/

/*
 * Trivial split: half of entries will be placed on one page
 * and another half - to another
 */
static void
tbox_fallbackSplit(GistEntryVector *entryvec, GIST_SPLITVEC *v)
{
	OffsetNumber i, maxoff;
	TBOX *unionL = NULL, *unionR = NULL;
	int nbytes;

	maxoff = entryvec->n - 1;

	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_nleft = v->spl_nright = 0;

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		TBOX *cur = DatumGetTboxP(entryvec->vector[i].key);
		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			v->spl_left[v->spl_nleft] = i;
			if (unionL == NULL)
			{
				unionL = (TBOX *) palloc0(sizeof(TBOX));
				*unionL = *cur;
			}
			else
				adjustBox(unionL, cur);

			v->spl_nleft++;
		}
		else
		{
			v->spl_right[v->spl_nright] = i;
			if (unionR == NULL)
			{
				unionR = (TBOX *) palloc0(sizeof(TBOX));
				*unionR = *cur;
			}
			else
				adjustBox(unionR, cur);

			v->spl_nright++;
		}
	}

	v->spl_ldatum = PointerGetDatum(unionL);
	v->spl_rdatum = PointerGetDatum(unionR);
}

/*
 * Represents information about an entry that can be placed to either group
 * without affecting overlap over selected axis ("common entry").
 */
typedef struct
{
	/* Index of entry in the initial array */
	int			index;
	/* Delta between penalties of entry insertion into different groups */
	double		delta;
} CommonEntry;

/*
 * Context for g_tbox_consider_split. Contains information about currently
 * selected split and some general information.
 */
typedef struct
{
	int			entriesCount;	/* total number of entries being split */
	TBOX		boundingBox;	/* minimum bounding box across all entries */

	/* Information about currently selected split follows */

	bool		first;			/* true if no split was selected yet */

	double		leftUpper;		/* upper bound of left interval */
	double		rightLower;		/* lower bound of right interval */

	float4		ratio;
	float4		overlap;
	int			dim;			/* axis of this split */
	double		range;			/* width of general MBR projection to the
								 * selected axis */
} ConsiderSplitContext;

/*
 * Interval represents projection of box to axis.
 */
typedef struct
{
	double		lower,
				upper;
} SplitInterval;

/*
 * Interval comparison function by lower bound of the interval;
 */
static int
interval_cmp_lower(const void *i1, const void *i2)
{
	double		lower1 = ((const SplitInterval *) i1)->lower,
				lower2 = ((const SplitInterval *) i2)->lower;

	return float8_cmp_internal(lower1, lower2);
}

/*
 * Interval comparison function by upper bound of the interval;
 */
static int
interval_cmp_upper(const void *i1, const void *i2)
{
	double		upper1 = ((const SplitInterval *) i1)->upper,
				upper2 = ((const SplitInterval *) i2)->upper;

	return float8_cmp_internal(upper1, upper2);
}

/*
 * Replace negative (or NaN) value with zero.
 */
static inline float
non_negative(float val)
{
	if (val >= 0.0f)
		return val;
	else
		return 0.0f;
}

/*
 * Consider replacement of currently selected split with the better one.
 */
static inline void
g_tbox_consider_split(ConsiderSplitContext *context, int dimNum,
	double rightLower, int minLeftCount, double leftUpper, int maxLeftCount)
{
	int			leftCount,
				rightCount;
	float4		ratio,
				overlap;
	double		range;

	/*
	 * Calculate entries distribution ratio assuming most uniform distribution
	 * of common entries.
	 */
	if (minLeftCount >= (context->entriesCount + 1) / 2)
	{
		leftCount = minLeftCount;
	}
	else
	{
		if (maxLeftCount <= context->entriesCount / 2)
			leftCount = maxLeftCount;
		else
			leftCount = context->entriesCount / 2;
	}
	rightCount = context->entriesCount - leftCount;

	/*
	 * Ratio of split - quotient between size of lesser group and total
	 * entries count.
	 */
	ratio = ((float4) Min(leftCount, rightCount)) /
		((float4) context->entriesCount);

	if (ratio > LIMIT_RATIO)
	{
		bool		selectthis = false;

		/*
		 * The ratio is acceptable, so compare current split with previously
		 * selected one. Between splits of one dimension we search for minimal
		 * overlap (allowing negative values) and minimal ration (between same
		 * overlaps. We switch dimension if find less overlap (non-negative)
		 * or less range with same overlap.
		 */
		if (dimNum == 0)
			range = context->boundingBox.xmax - context->boundingBox.xmin;
		else
			range = (double) (context->boundingBox.tmax - context->boundingBox.tmin);

		overlap = (leftUpper - rightLower) / range;

		/* If there is no previous selection, select this */
		if (context->first)
			selectthis = true;
		else if (context->dim == dimNum)
		{
			/*
			 * Within the same dimension, choose the new split if it has a
			 * smaller overlap, or same overlap but better ratio.
			 */
			if (overlap < context->overlap ||
				(overlap == context->overlap && ratio > context->ratio))
				selectthis = true;
		}
		else
		{
			/*
			 * Across dimensions, choose the new split if it has a smaller
			 * *non-negative* overlap, or same *non-negative* overlap but
			 * bigger range. This condition differs from the one described in
			 * the article. On the datasets where leaf MBRs don't overlap
			 * themselves, non-overlapping splits (i.e. splits which have zero
			 * *non-negative* overlap) are frequently possible. In this case
			 * splits tends to be along one dimension, because most distant
			 * non-overlapping splits (i.e. having lowest negative overlap)
			 * appears to be in the same dimension as in the previous split.
			 * Therefore MBRs appear to be very prolonged along another
			 * dimension, which leads to bad search performance. Using range
			 * as the second split criteria makes MBRs more quadratic. Using
			 * *non-negative* overlap instead of overlap as the first split
			 * criteria gives to range criteria a chance to matter, because
			 * non-overlapping splits are equivalent in this criteria.
			 */
			if (non_negative(overlap) < non_negative(context->overlap) ||
				(range > context->range &&
				 non_negative(overlap) <= non_negative(context->overlap)))
				selectthis = true;
		}

		if (selectthis)
		{
			/* save information about selected split */
			context->first = false;
			context->ratio = ratio;
			context->range = range;
			context->overlap = overlap;
			context->rightLower = rightLower;
			context->leftUpper = leftUpper;
			context->dim = dimNum;
		}
	}
}

/*
 * Compare common entries by their deltas.
 * (We assume the deltas can't be NaN.)
 */
static int
common_entry_cmp(const void *i1, const void *i2)
{
	double delta1 = ((const CommonEntry *) i1)->delta,
		delta2 = ((const CommonEntry *) i2)->delta;

	if (delta1 < delta2)
		return -1;
	else if (delta1 > delta2)
		return 1;
	else
		return 0;
}

/*
 * --------------------------------------------------------------------------
 * Double sorting split algorithm. This is used for both boxes and points.
 *
 * The algorithm finds split of boxes by considering splits along each axis.
 * Each entry is first projected as an interval on the X-axis, and different
 * ways to split the intervals into two groups are considered, trying to
 * minimize the overlap of the groups. Then the same is repeated for the
 * Y-axis, and the overall best split is chosen. The quality of a split is
 * determined by overlap along that axis and some other criteria (see
 * g_tbox_consider_split).
 *
 * After that, all the entries are divided into three groups:
 *
 * 1) Entries which should be placed to the left group
 * 2) Entries which should be placed to the right group
 * 3) "Common entries" which can be placed to any of groups without affecting
 *	  of overlap along selected axis.
 *
 * The common entries are distributed by minimizing penalty.
 *
 * For details see:
 * "A new double sorting-based node splitting algorithm for R-tree", A. Korotkov
 * http://syrcose.ispras.ru/2011/files/SYRCoSE2011_Proceedings.pdf#page=36
 * --------------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(gist_tbox_picksplit);

Datum
gist_tbox_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber i,
				maxoff;
	ConsiderSplitContext context;
	TBOX	   *box,
			   *leftBox,
			   *rightBox;
	int			dim,
				commonEntriesCount;
	SplitInterval *intervalsLower,
			   *intervalsUpper;
	CommonEntry *commonEntries;
	int			nentries;

	memset(&context, 0, sizeof(ConsiderSplitContext));

	maxoff = entryvec->n - 1;
	nentries = context.entriesCount = maxoff - FirstOffsetNumber + 1;

	/* Allocate arrays for intervals along axes */
	intervalsLower = (SplitInterval *) palloc(nentries * sizeof(SplitInterval));
	intervalsUpper = (SplitInterval *) palloc(nentries * sizeof(SplitInterval));

	/*
	 * Calculate the overall minimum bounding box over all the entries.
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		box = DatumGetTboxP(entryvec->vector[i].key);
		if (i == FirstOffsetNumber)
			context.boundingBox = *box;
		else
			adjustBox(&context.boundingBox, box);
	}

	/*
	 * Iterate over axes for optimal split searching.
	 */
	context.first = true;		/* nothing selected yet */
	for (dim = 0; dim < 2; dim++)
	{
		double		leftUpper,
					rightLower;
		int			i1,
					i2;

		/* Project each entry as an interval on the selected axis. */
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			box = DatumGetTboxP(entryvec->vector[i].key);
			if (dim == 0)
			{
				intervalsLower[i - FirstOffsetNumber].lower = box->xmin;
				intervalsLower[i - FirstOffsetNumber].upper = box->xmax;
			}
			else
			{
				intervalsLower[i - FirstOffsetNumber].lower = box->tmin;
				intervalsLower[i - FirstOffsetNumber].upper = box->tmax;
			}
		}

		/*
		 * Make two arrays of intervals: one sorted by lower bound and another
		 * sorted by upper bound.
		 */
		memcpy(intervalsUpper, intervalsLower,
			   sizeof(SplitInterval) * nentries);
		qsort(intervalsLower, nentries, sizeof(SplitInterval),
			  interval_cmp_lower);
		qsort(intervalsUpper, nentries, sizeof(SplitInterval),
			  interval_cmp_upper);

		/*----
		 * The goal is to form a left and right interval, so that every entry
		 * interval is contained by either left or right interval (or both).
		 *
		 * For example, with the intervals (0,1), (1,3), (2,3), (2,4):
		 *
		 * 0 1 2 3 4
		 * +-+
		 *	 +---+
		 *	   +-+
		 *	   +---+
		 *
		 * The left and right intervals are of the form (0,a) and (b,4).
		 * We first consider splits where b is the lower bound of an entry.
		 * We iterate through all entries, and for each b, calculate the
		 * smallest possible a. Then we consider splits where a is the
		 * upper bound of an entry, and for each a, calculate the greatest
		 * possible b.
		 *
		 * In the above example, the first loop would consider splits:
		 * b=0: (0,1)-(0,4)
		 * b=1: (0,1)-(1,4)
		 * b=2: (0,3)-(2,4)
		 *
		 * And the second loop:
		 * a=1: (0,1)-(1,4)
		 * a=3: (0,3)-(2,4)
		 * a=4: (0,4)-(2,4)
		 */

		/*
		 * Iterate over lower bound of right group, finding smallest possible
		 * upper bound of left group.
		 */
		i1 = 0;
		i2 = 0;
		rightLower = intervalsLower[i1].lower;
		leftUpper = intervalsUpper[i2].lower;
		while (true)
		{
			/*
			 * Find next lower bound of right group.
			 */
			while (i1 < nentries &&
				   FLOAT8_EQ(rightLower, intervalsLower[i1].lower))
			{
				if (FLOAT8_LT(leftUpper, intervalsLower[i1].upper))
					leftUpper = intervalsLower[i1].upper;
				i1++;
			}
			if (i1 >= nentries)
				break;
			rightLower = intervalsLower[i1].lower;

			/*
			 * Find count of intervals which anyway should be placed to the
			 * left group.
			 */
			while (i2 < nentries &&
				   FLOAT8_LE(intervalsUpper[i2].upper, leftUpper))
				i2++;

			/*
			 * Consider found split.
			 */
			g_tbox_consider_split(&context, dim, rightLower, i1, leftUpper, i2);
		}

		/*
		 * Iterate over upper bound of left group finding greatest possible
		 * lower bound of right group.
		 */
		i1 = nentries - 1;
		i2 = nentries - 1;
		rightLower = intervalsLower[i1].upper;
		leftUpper = intervalsUpper[i2].upper;
		while (true)
		{
			/*
			 * Find next upper bound of left group.
			 */
			while (i2 >= 0 && FLOAT8_EQ(leftUpper, intervalsUpper[i2].upper))
			{
				if (FLOAT8_GT(rightLower, intervalsUpper[i2].lower))
					rightLower = intervalsUpper[i2].lower;
				i2--;
			}
			if (i2 < 0)
				break;
			leftUpper = intervalsUpper[i2].upper;

			/*
			 * Find count of intervals which anyway should be placed to the
			 * right group.
			 */
			while (i1 >= 0 && FLOAT8_GE(intervalsLower[i1].lower, rightLower))
				i1--;

			/*
			 * Consider found split.
			 */
			g_tbox_consider_split(&context, dim,
								 rightLower, i1 + 1, leftUpper, i2 + 1);
		}
	}

	/*
	 * If we failed to find any acceptable splits, use trivial split.
	 */
	if (context.first)
	{
		tbox_fallbackSplit(entryvec, v);
		PG_RETURN_POINTER(v);
	}

	/*
	 * Ok, we have now selected the split across one axis.
	 *
	 * While considering the splits, we already determined that there will be
	 * enough entries in both groups to reach the desired ratio, but we did
	 * not memorize which entries go to which group. So determine that now.
	 */

	/* Allocate vectors for results */
	v->spl_left = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_right = (OffsetNumber *) palloc(nentries * sizeof(OffsetNumber));
	v->spl_nleft = 0;
	v->spl_nright = 0;

	/* Allocate bounding boxes of left and right groups */
	leftBox = palloc0(sizeof(TBOX));
	rightBox = palloc0(sizeof(TBOX));

	/*
	 * Allocate an array for "common entries" - entries which can be placed to
	 * either group without affecting overlap along selected axis.
	 */
	commonEntriesCount = 0;
	commonEntries = (CommonEntry *) palloc(nentries * sizeof(CommonEntry));

	/* Helper macros to place an entry in the left or right group */
#define PLACE_LEFT(box, off)					\
	do {										\
		if (v->spl_nleft > 0)					\
			adjustBox(leftBox, box);			\
		else									\
			*leftBox = *(box);					\
		v->spl_left[v->spl_nleft++] = off;		\
	} while(0)

#define PLACE_RIGHT(box, off)					\
	do {										\
		if (v->spl_nright > 0)					\
			adjustBox(rightBox, box);			\
		else									\
			*rightBox = *(box);					\
		v->spl_right[v->spl_nright++] = off;	\
	} while(0)

	/*
	 * Distribute entries which can be distributed unambiguously, and collect
	 * common entries.
	 */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		double		lower,
					upper;

		/*
		 * Get upper and lower bounds along selected axis.
		 */
		box = DatumGetTboxP(entryvec->vector[i].key);
		if (context.dim == 0)
		{
			lower = box->xmin;
			upper = box->xmax;
		}
		else
		{
			lower = box->tmin;
			upper = box->tmax;
		}

		if (FLOAT8_LE(upper, context.leftUpper))
		{
			/* Fits to the left group */
			if (FLOAT8_GE(lower, context.rightLower))
			{
				/* Fits also to the right group, so "common entry" */
				commonEntries[commonEntriesCount++].index = i;
			}
			else
			{
				/* Doesn't fit to the right group, so join to the left group */
				PLACE_LEFT(box, i);
			}
		}
		else
		{
			/*
			 * Each entry should fit on either left or right group. Since this
			 * entry didn't fit on the left group, it better fit in the right
			 * group.
			 */
			Assert(FLOAT8_GE(lower, context.rightLower));

			/* Doesn't fit to the left group, so join to the right group */
			PLACE_RIGHT(box, i);
		}
	}

	/*
	 * Distribute "common entries", if any.
	 */
	if (commonEntriesCount > 0)
	{
		/*
		 * Calculate minimum number of entries that must be placed in both
		 * groups, to reach LIMIT_RATIO.
		 */
		int			m = ceil(LIMIT_RATIO * (double) nentries);

		/*
		 * Calculate delta between penalties of join "common entries" to
		 * different groups.
		 */
		for (i = 0; i < commonEntriesCount; i++)
		{
			box = DatumGetTboxP(entryvec->vector[commonEntries[i].index].key);
			commonEntries[i].delta = Abs(box_penalty(leftBox, box) -
										 box_penalty(rightBox, box));
		}

		/*
		 * Sort "common entries" by calculated deltas in order to distribute
		 * the most ambiguous entries first.
		 */
		qsort(commonEntries, commonEntriesCount, sizeof(CommonEntry), common_entry_cmp);

		/*
		 * Distribute "common entries" between groups.
		 */
		for (i = 0; i < commonEntriesCount; i++)
		{
			box = DatumGetTboxP(entryvec->vector[commonEntries[i].index].key);

			/*
			 * Check if we have to place this entry in either group to achieve
			 * LIMIT_RATIO.
			 */
			if (v->spl_nleft + (commonEntriesCount - i) <= m)
				PLACE_LEFT(box, commonEntries[i].index);
			else if (v->spl_nright + (commonEntriesCount - i) <= m)
				PLACE_RIGHT(box, commonEntries[i].index);
			else
			{
				/* Otherwise select the group by minimal penalty */
				if (box_penalty(leftBox, box) < box_penalty(rightBox, box))
					PLACE_LEFT(box, commonEntries[i].index);
				else
					PLACE_RIGHT(box, commonEntries[i].index);
			}
		}
	}

	v->spl_ldatum = PointerGetDatum(leftBox);
	v->spl_rdatum = PointerGetDatum(rightBox);
	PG_RETURN_POINTER(v);
}

/*****************************************************************************
 * Leaf-level consistent method for temporal points using a tbox
 *****************************************************************************/

/*
 * Leaf-level consistency for tboxes
 *
 * Since boxes do not distinguish between inclusive and exclusive bounds it is 
 * necessary to generalize the tests, e.g., 
 * left : (box1->xmax < box2->xmin) => (box1->xmax <= box2->xmin) 
 * e.g., to take into account left([a,b],(b,c])
 * right : (box1->xmin > box2->xmax) => (box1->xmin >= box2->xmax)
 * e.g., to take into account right((b,c],[a,b])
 * and similarly for before and after
 */
bool
index_leaf_consistent_tbox(TBOX *key, TBOX *query, StrategyNumber strategy)
{
	bool retval;
	
	switch (strategy)
	{
		case RTOverlapStrategyNumber:
			retval = overlaps_tbox_tbox_internal(key, query);
			break;
		case RTContainsStrategyNumber:
			retval = contains_tbox_tbox_internal(key, query);
			break;
		case RTContainedByStrategyNumber:
			retval = contained_tbox_tbox_internal(key, query);
			break;
		case RTSameStrategyNumber:
			retval = same_tbox_tbox_internal(key, query);
			break;
		case RTLeftStrategyNumber:
			retval = /* left_tbox_tbox_internal(key, query) */
				(key->xmax <= query->xmin); 
			break;
		case RTOverLeftStrategyNumber:
			retval = overleft_tbox_tbox_internal(key, query); 
			break;
		case RTRightStrategyNumber:
			retval = /* right_tbox_tbox_internal(key, query) */ 
				(key->xmin >= query->xmax); 
			break;
		case RTOverRightStrategyNumber:
			retval = overright_tbox_tbox_internal(key, query);
			break;
		case RTBeforeStrategyNumber:
			retval = /* before_tbox_tbox_internal(key, query) */
				(key->tmax <= query->tmin); 
			break;
		case RTOverBeforeStrategyNumber:
			retval = overbefore_tbox_tbox_internal(key, query); 
			break;
		case RTAfterStrategyNumber:
			retval = /* after_tbox_tbox_internal(key, query) */
				(key->tmin >= query->tmax); 
			break;
		case RTOverAfterStrategyNumber:
			retval = overafter_tbox_tbox_internal(key, query);
			break;			
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			retval = false;		/* keep compiler quiet */
			break;
	}
	return retval;
}
		
/*****************************************************************************
 * Internal-page consistent method for temporal numbers using a tbox.
 *
 * Should return false if for all data items x below entry, the predicate 
 * x op query must be false, where op is the oper corresponding to strategy 
 * in the pg_amop table.
 *****************************************************************************/

static bool
gist_internal_consistent_tbox(TBOX *key, TBOX *query, StrategyNumber strategy)
{
	bool retval;
	
	switch (strategy)
	{
		case RTOverlapStrategyNumber:
		case RTContainedByStrategyNumber:
			retval = overlaps_tbox_tbox_internal(key, query);
			break;
		case RTContainsStrategyNumber:
		case RTSameStrategyNumber:
			retval = contains_tbox_tbox_internal(key, query);
			break;
		case RTLeftStrategyNumber:
			retval = !overright_tbox_tbox_internal(key, query);
			break;
		case RTOverLeftStrategyNumber:
			retval = !right_tbox_tbox_internal(key, query);
			break;
		case RTRightStrategyNumber:
			retval = !overleft_tbox_tbox_internal(key, query);
			break;
		case RTOverRightStrategyNumber:
			retval = !left_tbox_tbox_internal(key, query);
			break;
		case RTBeforeStrategyNumber:
			retval = !overafter_tbox_tbox_internal(key, query);
			break;
		case RTOverBeforeStrategyNumber:
			retval = !after_tbox_tbox_internal(key, query);
			break;
		case RTAfterStrategyNumber:
			retval = !overbefore_tbox_tbox_internal(key, query);
			break;
		case RTOverAfterStrategyNumber:
			retval = !before_tbox_tbox_internal(key, query);
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
			retval = false;		/* keep compiler quiet */
			break;
	}
	return retval;
}

/*****************************************************************************
 * GiST consistent method for temporal numbers
 *****************************************************************************/

PG_FUNCTION_INFO_V1(gist_tnumber_consistent);

PGDLLEXPORT Datum
gist_tnumber_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	Oid subtype = PG_GETARG_OID(3);
	bool *recheck = (bool *) PG_GETARG_POINTER(4), result;
	TBOX *key = DatumGetTboxP(entry->key), query;
	
	/* 
	 * All tests are lossy since boxes do not distinghish between inclusive  
	 * and exclusive bounds. 
	 */
	*recheck = true;
	
	if (key == NULL)
		PG_RETURN_BOOL(false);
	
	/*
	 * Transform the query into a box setting which are the dimensions that
	 * must be taken into account by the operators.
	 */
	if (subtype == type_oid(T_INTRANGE))
	{
		RangeType *range = PG_GETARG_RANGE_P(1);
		if (range == NULL)
			PG_RETURN_BOOL(false);
		intrange_to_tbox_internal(&query, range);
		PG_FREE_IF_COPY(range, 1);
	}
	else if (subtype == type_oid(T_FLOATRANGE))
	{
		RangeType *range = PG_GETARG_RANGE_P(1);
		if (range == NULL)
			PG_RETURN_BOOL(false);
		floatrange_to_tbox_internal(&query, range);
		PG_FREE_IF_COPY(range, 1);
	}
	else if (subtype == type_oid(T_TBOX))
	{
		TBOX *box = PG_GETARG_TBOX_P(1);
		if (box == NULL)
			PG_RETURN_BOOL(false);
		query = *box;
	}
	else if (temporal_type_oid(subtype))
	{
		Temporal *temp = PG_GETARG_TEMPORAL(1);
		if (temp == NULL)
			PG_RETURN_BOOL(false);
		temporal_bbox(&query, temp);
		PG_FREE_IF_COPY(temp, 1);
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);
	
	if (GIST_LEAF(entry))
		result = index_leaf_consistent_tbox(key, &query, strategy);
	else
		result = gist_internal_consistent_tbox(key, &query, strategy);
	
	PG_RETURN_BOOL(result);	
}

/*****************************************************************************
 * Compress method for temporal numbers
 *****************************************************************************/

PG_FUNCTION_INFO_V1(gist_tnumber_compress);

PGDLLEXPORT Datum
gist_tnumber_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	if (entry->leafkey)
	{
		GISTENTRY *retval = palloc(sizeof(GISTENTRY));
		Temporal *temp = DatumGetTemporal(entry->key);
		TBOX *box = palloc0(sizeof(TBOX));
		temporal_bbox(box, temp);
		gistentryinit(*retval, PointerGetDatum(box),
			entry->rel, entry->page, entry->offset, false);
		PG_RETURN_POINTER(retval);
	}
	PG_RETURN_POINTER(entry);
}

/*****************************************************************************
 * Equality method
 * Returns true only when boxes are exactly the same.  We can't use fuzzy
 * comparisons here without breaking index consistency; therefore, this isn't
 * equivalent to box_same().
 *****************************************************************************/

PG_FUNCTION_INFO_V1(gist_tbox_same);

Datum
gist_tbox_same(PG_FUNCTION_ARGS)
{
	TBOX *b1 = PG_GETARG_TBOX_P(0);
	TBOX *b2 = PG_GETARG_TBOX_P(1);
	bool *result = (bool *) PG_GETARG_POINTER(2);

	if (b1 && b2)
		*result = (FLOAT8_EQ(b1->xmin, b2->xmin) &&
				   FLOAT8_EQ(b1->tmin, b2->tmin) &&
				   FLOAT8_EQ(b1->xmax, b2->xmax) &&
				   FLOAT8_EQ(b1->tmax, b2->tmax));
	else
		*result = (b1 == NULL && b2 == NULL);
	PG_RETURN_POINTER(result);
}

/*****************************************************************************/
