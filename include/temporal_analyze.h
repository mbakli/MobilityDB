/*****************************************************************************
 *
 * temporal_analyze.h
 *
 * Portions Copyright (c) 2019, Esteban Zimanyi, Mahmoud Sakr, Mohamed Bakli,
 * 		Universite Libre de Bruxelles
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef __TEMPORAL_ANALYZE_H__
#define __TEMPORAL_ANALYZE_H__

#include "temporal_analyze.h"

#include <postgres.h>
#include <catalog/pg_type.h>
#include <commands/vacuum.h>
#include <utils/rangetypes.h>
#include <parser/parse_oper.h>
#include <statistics/extended_stats_internal.h>

#include "period.h"
#include "tpoint_spatialfuncs.h"

/**
* The dimensions of temporal types our code can handle.
* We'll use this to determine which part of our types
* should have statistics
*/
#define TEMPORAL_STATISTIC	1
#define TNUMBER_STATISTIC	2
#define TPOINT_STATISTIC	3

/* Extra data for compute_stats function */
typedef struct {
    /* Information about array element type */
    Oid type_id;			/* element type's OID */
    Oid eq_opr;				/* default equality operator's OID */
    bool typbyval;			/* physical properties of element type */
    int16 typlen;
    char typalign;

    /* Information about the value part of array element */
    Oid value_type_id;		/* element type's OID */
    Oid value_eq_opr;		/* default equality operator's OID */
    bool value_typbyval;	/* physical properties of element type */
    int16 value_typlen;
    char value_typalign;

    /* Information about the temporal part of array element */
    Oid temporal_type_id;	/* element type's OID */
    Oid temporal_eq_opr;	/* default equality operator's OID */
    bool temporal_typbyval;	/* physical properties of element type */
    int16 temporal_typlen;
    char temporal_typalign;

    /*
     * Lookup data for element type's comparison and hash functions (these are
     * in the type's typcache entry, which we expect to remain valid over the
     * lifespan of the ANALYZE run)
     */
    FmgrInfo *cmp;
    FmgrInfo *hash;
    FmgrInfo *value_cmp;
    FmgrInfo *value_hash;
    FmgrInfo *temporal_cmp;
    FmgrInfo *temporal_hash;

    /* Saved state from std_typanalyze() */
    AnalyzeAttrComputeStatsFunc std_compute_stats;
    void *std_extra_data;
} TemporalArrayAnalyzeExtraData;

/* A hash table entry for the Lossy Counting algorithm */
typedef struct
{
    Datum		key; 	 	 	/* This is 'e' from the LC algorithm. */
    int			frequency; 	  /* This is 'f'. */
    int			delta; 	 	  /* And this is 'delta'. */
    int			last_container; /* For de-duplication of array elements. */
} TrackItem;

/* A hash table entry for distinct-elements counts */
typedef struct
{
    int			count; 	 	  /* Count of distinct elements in an array */
    int			frequency; 	  /* Number of arrays seen with this count */
} DECountItem;

/*
 * Extra information used by the default analysis routines
 */
typedef struct
{
    int         count;          /* # of duplicates */
    int         first;          /* values[] index of first occurrence */
} ScalarMCVItem;

typedef struct
{
    SortSupport ssup;
    int        *tupnoLink;
} CompareScalarsContext;

extern Datum temporal_analyze_internal(VacAttrStats *stats, int durationType, int temporalType);

/*****************************************************************************
 * Statistics information for Temporal types
 *****************************************************************************/

extern void temporal_info(VacAttrStats *stats);
extern void temporal_extra_info(VacAttrStats *stats, int durationType);

/*****************************************************************************
 * Statistics functions for TemporalInst type
 *****************************************************************************/

extern void temporalinst_compute_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
                                       int samplerows, double totalrows);
extern void tnumberinst_compute_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
                                      int samplerows, double totalrows);

/*****************************************************************************
 * Statistics functions for TemporalI type
 *****************************************************************************/

extern void temporali_compute_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
                                    int samplerows, double totalrows);
extern void tnumberi_compute_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
                                   int samplerows, double totalrows);

/*****************************************************************************
 * Statistics functions for Trajectory types (TemporalSeq and TemporalS)
 *****************************************************************************/

extern void temporals_compute_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
                                    int samplerows, double totalrows);
extern void tnumbers_compute_stats(VacAttrStats *stats, AnalyzeAttrFetchFunc fetchfunc,
                                   int samplerows, double totalrows);

/*****************************************************************************
 * Comparison functions for different data types
 *****************************************************************************/

extern uint32 element_hash_value(const void *key, Size keysize);
extern uint32 element_hash_temporal(const void *key, Size keysize);
extern int element_match(const void *key1, const void *key2, Size keysize);
extern int trackitem_compare_frequencies_desc(const void *e1, const void *e2);
extern int trackitem_compare_element(const void *e1, const void *e2);
extern int countitem_compare_count(const void *e1, const void *e2);
extern int element_compare(const void *key1, const void *key2);
extern uint32 generic_element_hash(const void *key, Size keysize, FmgrInfo * hash);
extern int period_bound_qsort_cmp(const void *a1, const void *a2);
extern int float8_qsort_cmp(const void *a1, const void *a2);
extern int range_bound_qsort_cmp(const void *a1, const void *a2);
extern int compare_scalars(const void *a, const void *b, void *arg);
extern int compare_mcvs(const void *a, const void *b);

/*****************************************************************************
 * Different functions used for 1D, 2D, and 3D types.
 *****************************************************************************/

extern HeapTuple remove_temporaldim(HeapTuple tuple, TupleDesc tupDesc, int attrNum, Oid attrtypid,
                                    bool geom, Datum value);
extern Period* get_temporal_bbox(Datum value, Oid oid);
extern TBOX get_tnumber_bbox(Datum value, Oid oid);
extern STBOX get_tpoint_bbox(Datum value, Oid oid);
extern void tbox_deserialize(TBOX box, RangeBound *lowerdim1, RangeBound *upperdim1,
                             PeriodBound *lowerdim2, PeriodBound *upperdim2);
extern void stbox_deserialize(STBOX box, RangeBound *lowerdim1, RangeBound *upperdim1,
                              RangeBound *lowerdim2, RangeBound *upperdim2,
                              RangeBound *lowerdim3, RangeBound *upperdim3,
                              PeriodBound *lowerdim4, PeriodBound *upperdim4);

#endif //MOBILITYDB_TEMPANALYZE_COMMON_UTILITIES_H

/*****************************************************************************/