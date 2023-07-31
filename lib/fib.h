/*
 *	BIRD Internet Routing Daemon -- Network prefix storage
 *
 *	(c) 1998--2000 Martin Mares <mj@ucw.cz>
 *	(c) 2022 Maria Matejka <mq@jmq.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#ifndef _BIRD_LIB_FIB_H_
#define _BIRD_LIB_FIB_H_

#include <stdatomic.h>

/*
 *	Generic data structure for storing network prefixes. Also used
 *	for the master routing table. Currently implemented as a hash
 *	table.
 *
 *	Available operations:
 *		- insertion of new entry
 *		- deletion of entry
 *		- searching for entry by network prefix
 *		- asynchronous retrieval of fib contents
 */

struct fib_node {
  atomic_uintptr_t next;		/* Next in hash chain */
  atomic_char sentinel;      /* If node is a sentinel and number of pointer to address*/
  net_addr addr[0];
};

struct fib_iterator {			/* See lib/slists.h for an explanation */
  atomic_uintptr_t *curr;		/* Current hash table entry */
  uint row;
};

struct node_memory
{
	struct fib_node *node;
	struct node_memory *next;
	struct node_memory *prev;
};



typedef void (*fib_init_fn)(void *);

struct fib {
  pool *fib_pool;			/* Pool holding all our data */
  slab *fib_slab;			/* Slab holding all fib nodes */
  atomic_uintptr_t* hash_table;		/* Node hash table */
  atomic_bool* reserved_row;    /* Row of reserved in the hazard pointer */
  atomic_uintptr_t** soft_links; /* Soft links used */
  struct node_memory *handovers; /* Hand hovers of softlinks */
  struct node_memory *handovers_end;
  atomic_uint hash_size;			/* Number of hash table entries (a power of two) */
  atomic_uint hash_order;			/* Binary logarithm of hash_size */
  atomic_uint hash_shift;			/* 32 - hash_order */
  atomic_uint hash_mask;			/* hash_size - 1 */
  uint addr_type;			/* Type of address data stored in fib (NET_*) */
  uint node_size;			/* FIB node size, 0 for nonuniform,    SIZE OF WHAT IS INSIDE -> usualy a net which contain a fib node)  look like {struct rte*, fib_node}*/
  uint node_offset;			/* Offset of fib_node struct inside of user data,   WITH OFFSETOF(), offset between user data and fib node  (usually the pointer rte) */
  atomic_uint entries;				/* Number of entries */
  atomic_uint entries_min, entries_max;	/* Entry count limits (else start rehashing) */
  fib_init_fn init;			/* Constructor */
  pthread_t t;
  atomic_bool resizing;			/* resizing in progress */
  char stopThread;
};

static inline void * fib_node_to_user(struct fib *f, struct fib_node *e)
{ return e ? (void *) ((char *) e - f->node_offset) : NULL; }

static inline struct fib_node * fib_user_to_node(struct fib *f, void *e)
{ return e ? (void *) ((char *) e + f->node_offset) : NULL; }


void printfib(struct fib *f);

uint reserve_row(struct fib *f);
void release_row(struct fib *f, uint row);
char getSentinel(atomic_uintptr_t *ptr);
int getFlag(atomic_uintptr_t *ptr);
uintptr_t getNextAddress(atomic_uintptr_t *ptr);
void *fib_get2(struct fib *f, const net_addr *a, int row); //For testing
void consistency_check(struct fib *f);


void fib_init(struct fib *f, pool *p, uint addr_type, uint node_size, uint node_offset, uint hash_order, fib_init_fn init);
void *fib_find(struct fib *, const net_addr *);	/* Find or return NULL if doesn't exist */
struct fib_node* fib_get_chain(struct fib *f, const net_addr *a, uint row); /* Find first node in linked list from hash table */
void *fib_get(struct fib *, const net_addr *);	/* Find or create new if nonexistent */
void *fib_route(struct fib *, const net_addr *); /* Longest-match routing lookup */
int fib_delete(struct fib *, void *);	/* Remove fib entry */
void fib_free(struct fib *);		/* Destroy the fib */
void fib_check(struct fib *);		/* Consistency check for debugging */

void fit_init(struct fib_iterator *, struct fib *); /* Internal functions, don't call */
struct fib_node *fit_get(struct fib *, struct fib_iterator *);
void fit_put(struct fib_iterator *, struct fib_node *);
void fit_put_next(struct fib *f, struct fib_iterator *i, struct fib_node *n, uint hpos);
void fit_put_end(struct fib_iterator *i);
void fit_copy(struct fib *f, struct fib_iterator *dst, struct fib_iterator *src);


#define FIB_WALK(fibv, type, z) do { 			\
  struct fib *f_ = (fibv); \ 
  uint row = reserve_row(f_);					\
  atomic_uintptr_t *curr = &(f_->soft_links[row][0]); \
	atomic_store(curr, atomic_load(&(f_->hash_table[0])));	\
	type *z;						\
	while (atomic_load(curr)){					\
    if (getSentinel(curr) || getFlag(curr)) {\
      atomic_store(curr, getNextAddress(curr));\
      continue;\
    }\
    z = fib_node_to_user(f_, (struct fib_node*)atomic_load(curr));\
    do 
      

#define FIB_WALK_END  \
  while (0); \
  atomic_store(curr, getNextAddress(curr)); }\
  release_row(f_, row); \
} while(0); 

#define FIB_ITERATE_INIT(it, fib) fit_init(it, fib)

#define FIB_ITERATE_START(fibv, it, type, z) do {		\
  struct fib *f_ = (fibv); \ 
  struct fib_iterator *it_ = (it); \
	type *z;						\
	while (atomic_load(it_->curr)){					\
    if (getSentinel(it_->curr) || getFlag(it_->curr)) {\
      atomic_store(it_->curr, getNextAddress(it_->curr));\
      continue;\
    }\
    z = fib_node_to_user(f_, (struct fib_node*)atomic_load(it_->curr));\
    do

#define FIB_ITERATE_END \
  while (0); \
  if (atomic_load(it_->curr)) {\
  atomic_store(it_->curr, getNextAddress(it_->curr));} }\
  release_row(f_, it_->row); \
} while(0); 

#define FIB_ITERATE_PUT(it) fit_put(it, NULL)

#define FIB_ITERATE_PUT_NEXT(it, fib) fit_put_next(fib, it, NULL, 0)

#define FIB_ITERATE_PUT_END(it) fit_put_end(it);

#define FIB_ITERATE_UNLINK(it, fib) fit_get(fib, it)

#define FIB_ITERATE_COPY(dst, src, fib) fit_copy(fib, dst, src)

#endif
