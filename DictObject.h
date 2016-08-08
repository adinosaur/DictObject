//
// Created by dinosaur on 16-7-28.
//

#ifndef DMLIB_DICTOBJECT_H
#define DMLIB_DICTOBJECT_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WIN32
	typedef long ssize_t;
#endif

/* Dictionary object type -- mapping from hashable object to object */

/* The distribution includes a separate file, Objects/dictnotes.txt,
   describing explorations into dictionary design and optimization.
   It covers typical dictionary use patterns, the parameters for
   tuning dictionaries, and several ideas for possible optimizations.
*/

extern struct DictObject;

/* DictObject New and Dealloc */

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

/* DictObject method */

void * Dict_GetItem(DictObject *mp, void *key);
int Dict_SetItem(DictObject *mp, void *key, void *item);
int Dict_DelItem(DictObject *mp, void *key);
void Dict_Clear(void *mp);
int Dict_Next(DictObject *mp, ssize_t *pos, void **key, void **value);

#define DICT_GET_SIZE(op) (((DictObject *)(op))->ma_used)

/* hash function */
long int_hash(void *);

#ifdef __cplusplus
}
#endif

#endif //DMLIB_DICTOBJECT_H
