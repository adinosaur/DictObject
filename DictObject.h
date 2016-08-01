//
// Created by dinosaur on 16-7-28.
//

#ifndef DMLIB_DICTOBJECT_H
#define DMLIB_DICTOBJECT_H

#include <bits/types.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DICT_OBJ_DEBUG
#define DICT_OBJ_DEBUG
struct DictObjNode;
#endif

/* Dictionary object type -- mapping from hashable object to object */

/* The distribution includes a separate file, Objects/dictnotes.txt,
   describing explorations into dictionary design and optimization.
   It covers typical dictionary use patterns, the parameters for
   tuning dictionaries, and several ideas for possible optimizations.
*/

/*
There are three kinds of slots in the table:

1. Unused.  me_key == me_value == NULL
   Does not hold an active (key, value) pair now and never did.  Unused can
   transition to Active upon key insertion.  This is the only case in which
   me_key is NULL, and is each slot's initial state.

2. Active.  me_key != NULL and me_key != dummy and me_value != NULL
   Holds an active (key, value) pair.  Active can transition to Dummy upon
   key deletion.  This is the only case in which me_value != NULL.

3. Dummy.  me_key == dummy and me_value == NULL
   Previously held an active (key, value) pair, but that was deleted and an
   active pair has not yet overwritten the slot.  Dummy can transition to
   Active upon key insertion.  Dummy slots cannot be made Unused again
   (cannot have me_key set to NULL), else the probe sequence in case of
   collision would have no way to know they were once active.

Note: .popitem() abuses the me_hash field of an Unused or Dummy slot to
hold a search finger.  The me_hash field of Unused or Dummy slots has no
meaning otherwise.
*/

/* PyDict_MINSIZE is the minimum size of a dictionary.  This many slots are
 * allocated directly in the dict object (in the ma_smalltable member).
 * It must be a power of 2, and at least 4.  8 allows dicts with no more
 * than 5 active entries to live in ma_smalltable (and so avoid an
 * additional malloc); instrumentation suggested this suffices for the
 * majority of dicts (consisting mostly of usually-small instance dicts and
 * usually-small dicts created to pass keyword arguments).
 */
#define Dict_MINSIZE 8

typedef struct {
    /* Cached hash code of me_key.  Note that hash codes are C longs.
     * We have to use Py_ssize_t instead because dict_popitem() abuses
     * me_hash to hold a search finger.
     */
    ssize_t me_hash;
    void *me_key;
    void *me_value;
} DictEntry;

/*
To ensure the lookup algorithm terminates, there must be at least one Unused
slot (NULL key) in the table.
The value ma_fill is the number of non-NULL keys (sum of Active and Dummy);
ma_used is the number of non-NULL, non-dummy keys (== the number of non-NULL
values == the number of Active items).
To avoid slowing down lookups on a near-full table, we resize the table when
it's two-thirds full.
*/
typedef struct _dictobject DictObject;
struct _dictobject {
    ssize_t ma_fill;  /* # Active + # Dummy */
    ssize_t ma_used;  /* # Active */

    /* The table contains ma_mask + 1 slots, and that's a power of 2.
     * We store the mask instead of the size because the mask is more
     * frequently needed.
     */
    ssize_t ma_mask;

    /* ma_table points to ma_smalltable for small tables, else to
     * additional malloc'ed memory.  ma_table is never NULL!  This rule
     * saves repeated runtime null-tests in the workhorse getitem and
     * setitem calls.
     */
    DictEntry *ma_table;
    DictEntry *(*ma_lookup)(DictObject *mp, void *key, long hash);
    long (*ma_hash)(void*);

    /* for debug */
#ifdef DICT_OBJ_DEBUG
    DictObjNode *ma_node;
#endif

    DictEntry ma_smalltable[Dict_MINSIZE];
};

#ifdef DICT_OBJ_DEBUG
DictObject* _DictDebug_New(long(*)(void*), const char*, unsigned int, const char*);
int _DictDebug_Dealloc(DictObject*);
#define Dict_New(hashfun) (_DictDebug_New((hashfun), (__FILE__), (__LINE__), (__func__)))
#define Dict_Dealloc _DictDebug_Dealloc
#else
DictObject* _Dict_New(long(*hash)(void*));
int _Dict_Dealloc(DictObject*);
#define Dict_New _Dict_New
#define Dict_Dealloc _Dict_Dealloc
#endif

void * Dict_GetItem(DictObject *mp, void *key);
int Dict_SetItem(DictObject *mp, void *key, void *item);
int Dict_DelItem(DictObject *mp, void *key);
void Dict_Clear(void *mp);
int Dict_Next(DictObject *mp, ssize_t *pos, void **key, void **value);
int _Dict_Next(DictObject *mp, ssize_t *pos, void **key, void **value, long *hash);

#define DICT_GET_SIZE(op) (((DictObject *)(op))->ma_used)

#ifdef __cplusplus
}
#endif

#endif //DMLIB_DICTOBJECT_H
