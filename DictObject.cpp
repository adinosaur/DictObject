//
// Created by dinosaur on 16-7-28.
//

#include <assert.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>

#include "DictObject.h"


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
struct DictObject {
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
	long(*ma_hash)(void*);

	/* for debug */
#ifdef DICT_OBJ_DEBUG
	DictObjNode *ma_node;
#endif

	DictEntry ma_smalltable[Dict_MINSIZE];
};

struct DictObjNode;
struct DictObjNode {
    DictObject* obj;
    const char* file_str;
    const char* func_str;
    unsigned int line_no;
    DictObjNode* prev;
    DictObjNode* next;
};

/* 保存所有DictObject对象 */
static DictObjNode* obj_list = NULL;

#define DICT_IS_MEMLEAK() (((obj_list) != NULL))

/* See large comment block below.  This must be >= 1. */
#define PERTURB_SHIFT 5

/* Object used as dummy key to fill deleted entries */
static void *dummy = (void*) ("<dummy key>");

/* Initialization macros.
   There are two ways to create a dict:  PyDict_New() is the main C API
   function, and the tp_new slot maps to dict_new().  In the latter case we
   can save a little time over what PyDict_New does because it's guaranteed
   that the PyDictObject struct is already zeroed out.
   Everyone except dict_new() should use EMPTY_TO_MINSIZE (unless they have
   an excellent reason not to).
*/

#define INIT_NONZERO_DICT_SLOTS(mp) do {                                \
    (mp)->ma_table = (mp)->ma_smalltable;                               \
    (mp)->ma_mask = Dict_MINSIZE - 1;                                   \
    } while(0)

#define EMPTY_TO_MINSIZE(mp) do {                                       \
    memset((mp)->ma_smalltable, 0, sizeof((mp)->ma_smalltable));        \
    (mp)->ma_used = (mp)->ma_fill = 0;                                  \
    INIT_NONZERO_DICT_SLOTS(mp);                                        \
    } while(0)

static DictEntry* lookdict(DictObject *mp, void *key, register long hash);

#ifdef DICT_OBJ_DEBUG
DictObject*
_DictDebug_New(long(*hash)(void*),
               const char *file, unsigned int line,const char *function)
{
    register DictObject *mp;
    mp = (DictObject*) malloc(sizeof(DictObject));
    if (mp == NULL)
        return NULL;
    EMPTY_TO_MINSIZE(mp);
    mp->ma_lookup = lookdict;
    mp->ma_hash = hash;
    /* 将创建的DictObject对象插入obj_list中 */
    DictObjNode* np = (DictObjNode*) malloc(sizeof(DictObjNode));
    assert(np != NULL);
    np->obj = mp;
    np->file_str = file;
    np->func_str = function;
    np->line_no = line;
    if (obj_list == NULL) {
        np->prev = NULL;
        np->next = NULL;
    } else {
        np->prev = NULL;
        np->next = obj_list->next;
        obj_list->prev = np;
    }
    obj_list = np;
    mp->ma_node = np;
    return mp;
}

#else
DictObject *
_Dict_New(long(*hash)(void*))
{
    register DictObject *mp;
    mp = (DictObject*) malloc(sizeof(DictObject));
    if (mp == NULL)
        return NULL;
    EMPTY_TO_MINSIZE(mp);
    mp->ma_lookup = lookdict;
    mp->ma_hash = hash;
    return mp;
}

#endif

/*
The basic lookup function used by all operations.
This is based on Algorithm D from Knuth Vol. 3, Sec. 6.4.
Open addressing is preferred over chaining since the link overhead for
chaining would be substantial (100% with typical malloc overhead).

The initial probe index is computed as hash mod the table size. Subsequent
probe indices are computed as explained earlier.

All arithmetic on hash should ignore overflow.

(The details in this version are due to Tim Peters, building on many past
contributions by Reimer Behrends, Jyrki Alakuijala, Vladimir Marangozov and
Christian Tismer).

lookdict() is general-purpose, and may return NULL if (and only if) a
comparison raises an exception (this was new in Python 2.5).
lookdict_string() below is specialized to string keys, comparison of which can
never raise an exception; that function can never return NULL.  For both, when
the key isn't found a PyDictEntry* is returned for which the me_value field is
NULL; this is the slot in the dict at which the key would have been found, and
the caller can (if it wishes) add the <key, value> pair to the returned
PyDictEntry*.
*/
static DictEntry *
lookdict(DictObject *mp, void *key, register long hash)
{
    register size_t i;
    register size_t perturb;
    register DictEntry *freeslot;
    register size_t mask = (size_t)mp->ma_mask;
    DictEntry *ep0 = mp->ma_table;
    register DictEntry *ep;
    register int cmp;
    void *startkey;

    i = (size_t)hash & mask;
    ep = &ep0[i];
    if (ep->me_key == NULL || ep->me_key == key)
        return ep;

	freeslot = NULL;
    if (ep->me_key == dummy)
        freeslot = ep;

    /* In the loop, me_key == dummy is by far (factor of 100s) the
       least likely outcome, so test for that last. */
    for (perturb = hash; ; perturb >>= PERTURB_SHIFT) {
        /* 平方探测 */
        i = (i << 2) + i + perturb + 1;
        ep = &ep0[i & mask];
        if (ep->me_key == NULL)
            return freeslot == NULL ? ep : freeslot;
        if (ep->me_key == key)
            return ep;
        else if (ep->me_key == dummy && freeslot == NULL)
            freeslot = ep;
    }
    assert(0);          /* NOT REACHED */
    return 0;
}

/* Note that, for historical reasons, PyDict_GetItem() suppresses all errors
 * that may occur (originally dicts supported only string keys, and exceptions
 * weren't possible).  So, while the original intent was that a NULL return
 * meant the key wasn't present, in reality it can mean that, or that an error
 * (suppressed) occurred while computing the key's hash, or that some error
 * (suppressed) occurred when comparing keys in the dict's internal probe
 * sequence.  A nasty example of the latter is when a Python-coded comparison
 * function hits a stack-depth error, which can cause this to return NULL
 * even if the key is present.
 */
void *
Dict_GetItem(DictObject *mp, void *key)
{
    long hash;
    DictEntry *ep;
    assert(mp->ma_hash);
    hash = (mp->ma_hash)(key);
    if (hash == -1) {
        return NULL;
    }
    ep = (mp->ma_lookup)(mp, key, hash);
    if (ep == NULL) {
        return NULL;
    }
    return ep->me_value;
}

/*
Internal routine to insert a new item into the table.
Used both by the internal resize routine and by the public insert routine.
Eats a reference to key and one to value.
Returns -1 if an error occurred, or 0 on success.
*/
static int
insertdict(register DictObject *mp, void *key, long hash, void *value)
{
    void* old_value;
    register DictEntry *ep;
    assert(mp->ma_lookup != NULL);
    ep = mp->ma_lookup(mp, key, hash);
    if (ep == NULL) {
        return -1;
    }
    if (ep->me_value != NULL) {
        ep->me_value = value;
    } else {
        if (ep->me_key == NULL) {
            /* hash表里一个新的Entry被占用 */
            mp->ma_fill++;
        } else {
            assert(ep->me_key == dummy);
        }
        ep->me_key = key;
        ep->me_hash = hash;
        ep->me_value = value;
        mp->ma_used++;
    }
    return 0;
}

/*
Internal routine used by dictresize() to insert an item which is
known to be absent from the dict.  This routine also assumes that
the dict contains no deleted entries.  Besides the performance benefit,
using insertdict() in dictresize() is dangerous (SF bug #1456209).
Note that no refcounts are changed by this routine; if needed, the caller
is responsible for incref'ing `key` and `value`.
*/
static void
insertdict_clean(register DictObject *mp, void *key, long hash,
                 void *value)
{
    register size_t i;
    register size_t perturb;
    register size_t mask = (size_t)mp->ma_mask;
    DictEntry *ep0 = mp->ma_table;
    register DictEntry *ep;
    i = hash & mask;
    ep = &ep0[i];
    for (perturb = hash; ep->me_key != NULL; perturb >>= PERTURB_SHIFT) {
        i = (i << 2) + i + perturb + 1;
        ep = &ep0[i & mask];
    }
    assert(ep->me_value == NULL);
    mp->ma_fill++;
    ep->me_key = key;
    ep->me_hash = (ssize_t)hash;
    ep->me_value = value;
    mp->ma_used++;
}

/*
Restructure the table by allocating a new table and reinserting all
items again.  When entries have been deleted, the new table may
actually be smaller than the old one.
*/
static int
dictresize(DictObject *mp, ssize_t minused)
{
    ssize_t newsize;
    DictEntry *oldtable, *newtable, *ep;
    ssize_t i;
    int is_oldtable_malloced;
    DictEntry small_copy[Dict_MINSIZE];
    assert(minused >= 0);

    /* Find the smallest table size > minused. */
    for (newsize = Dict_MINSIZE; newsize <= minused && newsize > 0; newsize <<= 1)
        ;
    if (newsize <= 0) {
        return -1;
    }

    /* Get space for a new table. */
    oldtable = mp->ma_table;
    assert(oldtable != NULL);
    is_oldtable_malloced = oldtable != mp->ma_smalltable;

    if (newsize == Dict_MINSIZE) {
        /* A large table is shrinking, or we can't get any smaller. */
        newtable = mp->ma_smalltable;
        if (newtable == oldtable) {
            if (mp->ma_fill == mp->ma_used) {
                /* No dummies, so no point doing anything. */
                return 0;
            }
            /* We're not going to resize it, but rebuild the
               table anyway to purge old dummy entries.
               Subtle:  This is *necessary* if fill==size,
               as lookdict needs at least one virgin slot to
               terminate failing searches.  If fill < size, it's
               merely desirable, as dummies slow searches. */
            assert(mp->ma_fill > mp->ma_used);
            memcpy(small_copy, oldtable, sizeof(small_copy));
            oldtable = small_copy;
        }
    }
    else {
        newtable = (DictEntry*) malloc(sizeof(DictEntry) * newsize);
        if (newtable == NULL) {
            fprintf(stderr, "no enough memory");
            return -1;
        }
    }

    /* Make the dict empty, using the new table. */
    assert(newtable != oldtable);
    mp->ma_table = newtable;
    mp->ma_mask = newsize - 1;
    memset(newtable, 0, sizeof(DictEntry) * newsize);
    mp->ma_used = 0;
    i = mp->ma_fill;
    mp->ma_fill = 0;

    /* Copy the data over; this is refcount-neutral for active entries;
       dummy entries aren't copied over, of course */
    for (ep = oldtable; i > 0; ep++) {
        if (ep->me_value != NULL) {             /* active entry */
            --i;
            insertdict_clean(mp, ep->me_key, (long)ep->me_hash,
                             ep->me_value);
        }
        else if (ep->me_key != NULL) {          /* dummy entry */
            --i;
            assert(ep->me_key == dummy);
        }
        /* else key == value == NULL:  nothing to do */
    }
    if (is_oldtable_malloced)
        free(oldtable);
    return 0;
}

/* CAUTION: PyDict_SetItem() must guarantee that it won't resize the
 * dictionary if it's merely replacing the value for an existing key.
 * This means that it's safe to loop over a dictionary with PyDict_Next()
 * and occasionally replace a value -- but you can't insert new keys or
 * remove them.
 */
int
Dict_SetItem(register DictObject *op, void *key, void *value)
{
    register long hash;
    register ssize_t n_used;
    assert(key);
    assert(value);
    assert(op->ma_hash);

    hash = op->ma_hash(key);
    if (hash == -1)
        return -1;
    assert(op->ma_fill <= op->ma_mask);  /* at least one empty slot */
    n_used = op->ma_used;
    if (insertdict(op, key, hash, value) == -1)
        return -1;

    /* If we added a key, we can safely resize.  Otherwise just return!
     * If fill >= 2/3 size, adjust size.  Normally, this doubles or
     * quaduples the size, but it's also possible for the dict to shrink
     * (if ma_fill is much larger than ma_used, meaning a lot of dict
     * keys have been * deleted).
     *
     * Quadrupling the size improves average dictionary sparseness
     * (reducing collisions) at the cost of some memory and iteration
     * speed (which loops over every possible entry).  It also halves
     * the number of expensive resize operations in a growing dictionary.
     *
     * Very large dictionaries (over 50K items) use doubling instead.
     * This may help applications with severe memory constraints.
     */
    if (!(op->ma_used > n_used && op->ma_fill*3 >= (op->ma_mask+1)*2))
        return 0;
    return dictresize(op, (op->ma_used > 50000 ? 2 : 4) * op->ma_used);
}

int
Dict_DelItem(DictObject *op, void *key)
{
    register long hash;
    register DictEntry *ep;

    assert(key);
    assert(op->ma_hash);

    hash = op->ma_hash(key);
    if (hash == -1)
        return -1;
    ep = (op->ma_lookup)(op, key, hash);
    if (ep == NULL)
        return -1;
    if (ep->me_value == NULL) {
        return -1;
    }
    ep->me_key = dummy;
    ep->me_value = NULL;
    op->ma_used--;
    return 0;
}

void
Dict_Clear(DictObject *op)
{
    DictEntry *ep, *table;
    int table_is_malloced;
    ssize_t fill;
    DictEntry small_copy[Dict_MINSIZE];

    table = op->ma_table;
    assert(table != NULL);
    table_is_malloced = table != op->ma_smalltable;

    /* This is delicate.  During the process of clearing the dict,
     * decrefs can cause the dict to mutate.  To avoid fatal confusion
     * (voice of experience), we have to make the dict empty before
     * clearing the slots, and never refer to anything via mp->xxx while
     * clearing.
     */
    fill = op->ma_fill;
    if (table_is_malloced)
        EMPTY_TO_MINSIZE(op);

    else if (fill > 0) {
        /* It's a small table with something that needs to be cleared.
         * Afraid the only safe way is to copy the dict entries into
         * another small table first.
         */
        memcpy(small_copy, table, sizeof(small_copy));
        table = small_copy;
        EMPTY_TO_MINSIZE(op);
    }
    /* else it's a small table that's already empty */
    if (table_is_malloced)
        free(table);
}

/*
 * Iterate over a dict.  Use like so:
 *
 *     Py_ssize_t i;
 *     PyObject *key, *value;
 *     i = 0;   # important!  i should not otherwise be changed by you
 *     while (PyDict_Next(yourdict, &i, &key, &value)) {
 *              Refer to borrowed references in key and value.
 *     }
 *
 * CAUTION:  In general, it isn't safe to use PyDict_Next in a loop that
 * mutates the dict.  One exception:  it is safe if the loop merely changes
 * the values associated with the keys (but doesn't insert new keys or
 * delete keys), via PyDict_SetItem().
 */
int
Dict_Next(DictObject *op, ssize_t *ppos, void **pkey, void **pvalue)
{
    register ssize_t i;
    register ssize_t mask;
    register DictEntry *ep;

    i = *ppos;
    if (i < 0)
        return 0;
    ep = op->ma_table;
    mask = op->ma_mask;
    while (i <= mask && ep[i].me_value == NULL)
        i++;
    *ppos = i+1;
    if (i > mask)
        return 0;
    if (pkey)
        *pkey = ep[i].me_key;
    if (pvalue)
        *pvalue = ep[i].me_value;
    return 1;
}

/* Internal version of PyDict_Next that returns a hash value in addition to the key and value.*/
int
_Dict_Next(DictObject *op, ssize_t *ppos, void **pkey, void **pvalue, long *phash)
{
    register ssize_t i;
    register ssize_t mask;
    register DictEntry *ep;

    i = *ppos;
    if (i < 0)
        return 0;
    ep = op->ma_table;
    mask = op->ma_mask;
    while (i <= mask && ep[i].me_value == NULL)
        i++;
    *ppos = i+1;
    if (i > mask)
        return 0;
    *phash = (long)(ep[i].me_hash);
    if (pkey)
        *pkey = ep[i].me_key;
    if (pvalue)
        *pvalue = ep[i].me_value;
    return 1;
}

/*
 * 字典的析构函数，仅释放字典本身的内存，对存储在字典内的对象不做任何处理。
 * 因此，需要使用者在调用该函数前先释放字典内的对象。
 */

#ifdef DICT_OBJ_DEBUG
int
_DictDebug_Dealloc(DictObject* dict)
{
    if (dict == NULL)
        return 0;
    Dict_Clear(dict);
    DictObjNode* np = dict->ma_node;
    if (np == NULL) {
        fprintf(stderr, "dict object's ma_node is NULL");
        assert(0);
    }
    /* remove node from obj_list */
    assert(obj_list);
    if (np->prev == NULL && np->next != NULL) {
        /* len(obj_list) > 1 and remove head node */
        np->next->prev = NULL;
        obj_list = np->next;
    } else if (np->prev != NULL && np->next == NULL) {
        /* len(obj_list) > 1 and remove tail node */
        np->prev->next = NULL;
    } else if (np->prev == NULL && np->next == NULL) {
        /* len(obj_list) == 1 and remove the only node */
        obj_list = NULL;
    } else {
        np->prev->next = np->next;
        np->next->prev = np->prev;
    }
    free(np);
    free(dict);
    return 0;
}

#else
int
_Dict_Dealloc(DictObject* dict)
{
    if (dict == NULL)
        return 0;
    Dict_Clear(dict);
    free(dict);
    return 0;
}

#endif

/* hash functions */
long
int_hash(void *v)
{
    /* XXX If this is changed, you also need to change the way
       Python's long, float and complex types are hashed. */
    long x = (long)v;
    if (x == -1)
        x = -2;
    return x;
}

void
dict_test()
{
    DictObject* dict;
    DictObjNode* node;
    void *key, *value;
    ssize_t i;

    dict = Dict_New(int_hash);
    for (i = 1; i != 10; ++i) {
        Dict_SetItem(dict, (void*)i, (void*)i);
    }

    i = 0;
    while (Dict_Next(dict, &i, (void**)&key, (void**)&value)) {
        printf("key:(%d),value:(%d)\n", (long)key, (long)value);
    }

    for (i = 1; i != 10; ++i) {
        value = Dict_GetItem(dict, (void*)i);
        assert((ssize_t)value == i);
    }

    Dict_DelItem(dict, (void*)1);
    value = Dict_GetItem(dict, (void*)1);
    assert(!value);
    Dict_Dealloc(dict);

    if (obj_list != NULL) {
        for (node = obj_list; node != NULL; node = node->next) {
            fprintf(stderr, "dict memory leak in %s:%s:%d\n", node->file_str, node->func_str, node->line_no);
        }
    }
}