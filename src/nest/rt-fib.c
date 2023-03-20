/*
 *	BIRD -- Forwarding Information Base -- Data Structures
 *
 *	(c) 1998--2000 Martin Mares <mj@ucw.cz>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

/**
 * DOC: Forwarding Information Base
 *
 * FIB is a data structure designed for storage of routes indexed by their
 * network prefixes. It supports insertion, deletion, searching by prefix,
 * `routing' (in CIDR sense, that is searching for a longest prefix matching
 * a given IP address) and (which makes the structure very tricky to implement)
 * asynchronous reading, that is enumerating the contents of a FIB while other
 * modules add, modify or remove entries.
 *
 * Internally, each FIB is represented as a collection of nodes of type &fib_node
 * indexed using a sophisticated hashing mechanism.
 * We use two-stage hashing where we calculate a 16-bit primary hash key independent
 * on hash table size and then we just divide the primary keys modulo table size
 * to get a real hash key used for determining the bucket containing the node.
 * The lists of nodes in each bucket are sorted according to the primary hash
 * key, hence if we keep the total number of buckets to be a power of two,
 * re-hashing of the structure keeps the relative order of the nodes.
 *
 * To get the asynchronous reading consistent over node deletions, we need to
 * keep a list of readers for each node. When a node gets deleted, its readers
 * are automatically moved to the next node in the table.
 *
 * Basic FIB operations are performed by functions defined by this module,
 * enumerating of FIB contents is accomplished by using the FIB_WALK() macro
 * or FIB_ITERATE_START() if you want to do it asynchronously.
 *
 * For simple iteration just place the body of the loop between FIB_WALK() and
 * FIB_WALK_END(). You can't modify the FIB during the iteration (you can modify
 * data in the node, but not add or remove nodes).
 *
 * If you need more freedom, you can use the FIB_ITERATE_*() group of macros.
 * First, you initialize an iterator with FIB_ITERATE_INIT(). Then you can put
 * the loop body in between FIB_ITERATE_START() and FIB_ITERATE_END(). In
 * addition, the iteration can be suspended by calling FIB_ITERATE_PUT().
 * This'll link the iterator inside the FIB. While suspended, you may modify the
 * FIB, exit the current function, etc. To resume the iteration, enter the loop
 * again. You can use FIB_ITERATE_UNLINK() to unlink the iterator (while
 * iteration is suspended) in cases like premature end of FIB iteration.
 *
 * Note that the iterator must not be destroyed when the iteration is suspended,
 * the FIB would then contain a pointer to invalid memory. Therefore, after each
 * FIB_ITERATE_INIT() or FIB_ITERATE_PUT() there must be either
 * FIB_ITERATE_START() or FIB_ITERATE_UNLINK() before the iterator is destroyed.
 */

#undef LOCAL_DEBUG

#include "nest/bird.h"
#include "nest/route.h"
#include "lib/string.h"



#include <stdio.h>

/*
 * The FIB rehash values are maintaining FIB count between N/5 and 2N. What
 * does it mean?
 *
 * +------------+--------+---------+-----------+----------+-----------+
 * | Table size | Memory | Min cnt | net + rte |  Max cnt | net + rte |
 * +------------+--------+---------+-----------+----------+-----------+
 * |         1k |     8k |    0    |      0    |       2k |    192  k |
 * |         2k |    16k |  409    |     38.3k |       4k |    384  k |
 * |         4k |    32k |  819    |     76.8k |       8k |    768  k |
 * |         8k |    64k |    1.6k |    153.6k |      16k |      1.5M |
 * |        16k |   128k |    3.2k |    307.1k |      32k |      3  M |
 * |        32k |   256k |    6.4k |    614.3k |      64k |      6  M |
 * |        64k |   512k |   12.8k |      1.2M |     128k |     12  M |
 * |       128k |  1024k |   25.6k |      2.4M |     256k |     24  M |
 * |       256k |     2M |   51.2k |      4.8M |     512k |     48  M |
 * |       512k |     4M |  102.4k |      9.6M |       1M |     96  M |
 * |         1M |     8M |  204.8k |     19.2M |       2M |    192  M |
 * |         2M |    16M |  409.6k |     38.4M |       4M |    384  M |
 * |         4M |    32M |  819.2k |     76.8M |       8M |    768  M |
 * |         8M |    64M |    1.6M |    153.6M | infinity |  infinity |
 * +------------+--------+---------+-----------+----------+-----------+
 *
 * Table size	shows how many slots are in FIB table.
 * Memory	shows how much memory is eaten by FIB table.
 * Min cnt	minimal number of nets in table of given size
 * Max cnt	maximal number of nets in table of given size
 * net + rte	memory eaten by 1 net and one route in it for min cnt and max cnt
 *
 * Example: If we have 750,000 network entries in a table:
 * * the table size may be 512k if we have never had more
 * * the table size may be 1M or 2M if we at least happened to have more
 * * 256k is too small, 8M is too big
 *
 * When growing, rehash is done on demand so we do it on every power of 2.
 * When shrinking, rehash is done on delete which is done (in global tables)
 * in a scheduled event. Rehashing down 2 steps.
 *
 */

#define HASH_DEF_ORDER 10
#define HASH_HI_MARK *2
#define HASH_HI_STEP 1
#define HASH_HI_MAX 24
#define HASH_LO_MARK / 5
#define HASH_LO_STEP 2
#define HASH_LO_MIN 10

#define MAX_THREADS 32

static inline u32 fib_hash(struct fib *f, const net_addr *a);


int getParent(u32 bucket, u32 bucketSize){
    u32 parent = bucketSize;
    do {
        parent = parent >> 1;
    } while (parent > bucket);
    parent = bucket - parent;
	//printf("Parent of %u is %u\n", bucket, parent);
    return parent;
}

static uint reserve_row(struct fib *f)
{
	while (1)
	{
		for (int i = 0; i < MAX_THREADS; i++)
		{
			if (!atomic_exchange(&(f->reserved_row[i]), 1))
				return i;
		}
	}
}

static void release_row(struct fib *f, int row)
{
	if (row >= 32) printf("Error row\n");
	atomic_store(&(f->reserved_row[row]), 0);
}

static int setFlagTrue(atomic_uintptr_t* ptr){
    atomic_uintptr_t old; 
    atomic_store(&old, atomic_fetch_or(ptr, 1));
    return (atomic_load(&old) & 1) == 0;
}

u32 reverseBits(u32 num)
{
	u32 NO_OF_BITS = sizeof(num) * 8;
	u32 reverse_num = 0;
	u32 i;
	for (i = 0; i < NO_OF_BITS; i++)
	{
		if ((num & (1 << i)))
			reverse_num |= 1 << ((NO_OF_BITS - 1) - i);
	}
	return reverse_num;
}

u32 getHashFromSentinel(struct fib *f, atomic_uintptr_t *ptr)
{
	while (1)
	{
		for (unsigned int i = 0; i < atomic_load(&(f->hash_size)); i++)
		{
			if (atomic_load(&(f->hash_table[i])) == atomic_load(ptr))
				return i;
		}
	}
}

u32 getHash(struct fib *f, atomic_uintptr_t *ptr)
{
	struct fib_node *node = (struct fib_node *)atomic_load(ptr);
	if (atomic_load(&(node->sentinel)))
		return getHashFromSentinel(f, ptr);
	else
		return net_hash(&(node->addr[0]));
}

static uintptr_t getAddress(atomic_uintptr_t *ptr)
{
	return atomic_load(ptr) & ~1;
}

static uintptr_t getNextAddress(atomic_uintptr_t *ptr)
{
	struct fib_node *node = (struct fib_node *)atomic_load(ptr);
	return getAddress(&(node->next));
}

char getSentinel(atomic_uintptr_t *ptr)
{
	struct fib_node *node = (struct fib_node *)atomic_load(ptr);
	return (char)(atomic_load(&(node->sentinel)) & 1);
}






void printfib(struct fib *f)
{
	atomic_uintptr_t ptr = f->hash_table[0];

	while (atomic_load(&ptr) != 0)
	{
		struct fib_node *node = (struct fib_node *)atomic_load(&ptr);
		if (node->sentinel)
			printf("\nSentinel node: ");
		printf("%u ", getHash(f, &ptr));
		atomic_store(&ptr, getNextAddress(&ptr));
	}
	printf("\n");
}





static void freeNode(struct fib *f, atomic_uintptr_t *ptr, int row){
	//TODO
}










/**
 * fib_init - initialize a new FIB
 * @f: the FIB to be initialized (the structure itself being allocated by the caller)
 * @p: pool to allocate the nodes in
 * @node_size: node size to be used (each node consists of a standard header &fib_node
 * followed by user data)
 * @hash_order: initial hash order (a binary logarithm of hash table size), 0 to use default order
 * (recommended)
 * @init: pointer a function to be called to initialize a newly created node
 *
 * This function initializes a newly allocated FIB and prepares it for use.
 */
void fib_init(struct fib *f, pool *p, uint addr_type, uint node_size, uint node_offset, uint hash_order, fib_init_fn init)
{
	uint addr_length = net_addr_length[addr_type];

	if (!hash_order)
		hash_order = HASH_DEF_ORDER;
	f->fib_pool = p;
	f->fib_slab = addr_length ? sl_new(p, node_size + addr_length) : NULL;
	f->addr_type = addr_type;
	f->node_size = node_size;
	f->node_offset = node_offset;

	atomic_store(&(f->hash_order), hash_order);

	atomic_store(&(f->hash_size), 1 << atomic_load(&(f->hash_order)));
	atomic_store(&(f->hash_shift), 32 - atomic_load(&(f->hash_order)));
	atomic_store(&(f->hash_mask), atomic_load(&(f->hash_size)) - 1);
	if (f->hash_order > HASH_HI_MAX - HASH_HI_STEP)
		atomic_store(&(f->entries_max), ~0);
	else
		atomic_store(&(f->entries_max), atomic_load(&(f->hash_size)) HASH_HI_MARK);
	if (f->hash_order < HASH_LO_MIN + HASH_LO_STEP)
		atomic_store(&(f->entries_min), 0);
	else
		atomic_store(&(f->entries_min), atomic_load(&(f->hash_size)) HASH_LO_MARK);
	DBG("Allocating FIB hash of order %d: %d entries, %d low, %d high\n",
		atomic_load(&(f->hash_order)), atomic_load(&(f->hash_size)), atomic_load(&(f->entries_min)), atomic_load(&(f->entries_max)));

	atomic_store(&(f->hash_table), mb_alloc(f->fib_pool, atomic_load(&(f->hash_size)) * sizeof(atomic_uintptr_t)));
	// Put to zero
	for (uint i = 0; i < atomic_load(&(f->hash_size)); i++)
		atomic_store(&(f->hash_table[i]), 0);

	// Allocate the reserved row
	atomic_store(&(f->reserved_row), mb_alloc(f->fib_pool, sizeof(atomic_bool) * MAX_THREADS));
	for (uint i = 0; i < MAX_THREADS; i++)
		atomic_store(&(f->reserved_row[i]), 0);

	// Allocate the softlinks and handovers
	atomic_store(&(f->soft_links), mb_alloc(f->fib_pool, sizeof(atomic_uintptr_t *) * MAX_THREADS));
	atomic_store(&(f->hand_overs), mb_alloc(f->fib_pool, sizeof(atomic_uintptr_t *) * MAX_THREADS));
	for (uint i = 0; i < MAX_THREADS; i++)
	{
		atomic_store(&(f->soft_links[i]), mb_alloc(f->fib_pool, 2 * sizeof(atomic_uintptr_t)));
		atomic_store(&(f->hand_overs[i]), mb_alloc(f->fib_pool, 2 * sizeof(atomic_uintptr_t)));
		atomic_store(&(f->soft_links[i][0]), 0);
		atomic_store(&(f->soft_links[i][1]), 0);
		atomic_store(&(f->hand_overs[i][0]), 0);
		atomic_store(&(f->hand_overs[i][1]), 0);
	}

	atomic_store(&(f->entries), 0);
	atomic_store(&(f->entries_min), 0);
	f->init = init;

	atomic_store(&(f->resizing), 0);

	// Adding first node
	struct fib_node *b = mb_alloc(f->fib_pool, sizeof(struct fib_node));

	atomic_store(&(b->next), 0);
	atomic_store(&(b->sentinel), 1);
	atomic_store(&(f->hash_table[0]), (atomic_uintptr_t)b);
}

// Resize
static void
fib_rehash(struct fib *f, int step)
{
}

#define CAST(t) (const net_addr_##t *)
#define CAST2(t) (net_addr_##t *)

#define FIB_HASH(f, a, t) (net_hash_##t(CAST(t) a) >> atomic_load(&(f->hash_shift)))

#define FIB_FIND(f, a, t)                                       \
	({                                                          \
		struct fib_node *e = f->hash_table[FIB_HASH(f, a, t)];  \
		while (e && !net_equal_##t(CAST(t) e->addr, CAST(t) a)) \
			e = e->next;                                        \
		fib_node_to_user(f, e);                                 \
	})

#define FIB_INSERT(f, a, e, t)                                       \
	({                                                               \
		u32 h = net_hash_##t(CAST(t) a);                             \
		struct fib_node **ee = f->hash_table + (h >> f->hash_shift); \
		struct fib_node *g;                                          \
                                                                     \
		net_copy_##t(CAST2(t) e->addr, CAST(t) a);                   \
		e->next = *ee;                                               \
		*ee = e;                                                     \
	})

static inline u32
fib_hash(struct fib *f, const net_addr *a)
{
	/* Same as FIB_HASH() */
	return net_hash(a) >> f->hash_shift;
}

void *
fib_get_chain(struct fib *f, const net_addr *a)
{
	ASSERT(f->addr_type == a->type);

	struct fib_node *e = (struct fib_node *)atomic_load(&(f->hash_table[fib_hash(f, a)]));
	return e;
}



static void *
fib_insert2(struct fib *f, const net_addr *a, int row, u32 bucket)
{
	if (a)
	{
		ASSERT(f->addr_type == a->type);
		switch (f->addr_type)
		{
		case NET_IP4:
			break;
		case NET_IP6:
			break;
		case NET_VPN4:
			break;
		case NET_VPN6:
			break;
		case NET_ROA4:
			break;
		case NET_ROA6:
			break;
		case NET_FLOW4:
			break;
		case NET_FLOW6:
			break;
		case NET_IP6_SADR:
			break;
		case NET_MPLS:
			break;
		default:
			bug("invalid type");
		}
	}

	atomic_uintptr_t *curr = &(f->soft_links[row][0]);
	atomic_uintptr_t *succ = &(f->soft_links[row][1]);

	char sentinel;
	if (a)
		sentinel = 0;
	else
		sentinel = 1;

	u32 hash;
	if (sentinel)
		hash = bucket;
	else
		hash = net_hash(a);
	u32 starting_bucket;
	u32 key = reverseBits(hash);
	struct fib_node* new_node = NULL;
	atomic_uintptr_t expected;

	while (1)
	{
		// Checking in which bucket to insert
		if (sentinel)
			starting_bucket = getParent(hash, atomic_load(&(f->hash_size)));
		else
			starting_bucket = hash & atomic_load(&(f->hash_mask));
		
		if (starting_bucket >= atomic_load(&(f->hash_size)))
		{
			//printf("Error cause starting_bucket >= hash_size\n");
			//continue;
		}

		if (atomic_load(&(f->hash_table[starting_bucket])) == 0)
		{
			fib_insert2(f, NULL, row, starting_bucket);
		}

		//printf("Trying to add sentinel %d hash %u to bucket %u\n", (int) sentinel, hash, starting_bucket);

		atomic_store(curr, atomic_load(&(f->hash_table[starting_bucket])));

		if (atomic_load(curr) == 0)
		{
			// Need to wait for resize to end
			//printf("Exit cause curr is 0\n");
			continue;
		}

		atomic_store(succ, getNextAddress(curr));

		// Find the right place to insert the node
		while (atomic_load(succ) != 0 && reverseBits(getHash(f, succ)) < key)
		{
			atomic_store(curr, getNextAddress(curr));
			if (atomic_load(curr) == 0)
			{
				// Not found
				return NULL;
			}
			atomic_store(succ, getNextAddress(curr));
		}

		// If node is a sentinel, succ is the sentinel or a node with the same value
		if (sentinel)
		{
			if (atomic_load(succ) != 0 && getHash(f, succ) == hash && getSentinel(succ) == sentinel)
				return NULL;
		}
		else
		{
			// succ is a sentinel, skip once
			if (atomic_load(succ) != 0 && getHash(f, succ) == hash && getSentinel(succ))
			{
				atomic_store(curr, atomic_load(succ));
				atomic_store(succ, getNextAddress(curr));
			}
		}

		// If the node already exists, return 0
		if (atomic_load(succ) != 0 && getHash(f, succ) == hash && getSentinel(succ) == sentinel)
		{
			if (new_node != NULL){
				if (sentinel){
					mb_free(new_node);
				}
				else {
					mb_free(fib_node_to_user(f, new_node));
				}
			}
			return NULL;
		}

		if (new_node == NULL)
		{
			if (sentinel){
				new_node = mb_alloc(f->fib_pool, sizeof(struct fib_node));
				//if (new_node == NULL)
					//printf("Error allocating memory\n");
			}
			else {
				new_node = mb_alloc(f->fib_pool, a->length + f->node_size);
				//if (new_node == NULL)
					//printf("Error allocating memory\n");
				new_node = fib_user_to_node(f, new_node);
				net_copy(&(new_node->addr[0]), a);
			}
			atomic_store(&(new_node->sentinel), sentinel);
		}
		atomic_store(&(new_node->next), atomic_load(succ));
		atomic_store(&expected, atomic_load(succ));

		if (reverseBits(getHash(f, curr)) > key)
		{
			// I don't know why it manage to reach here
			//printf("Problem with key getting to far\n");
			continue;
		}


		if (atomic_compare_exchange_weak(&(((struct fib_node *)atomic_load(curr))->next), &expected, (atomic_uintptr_t)new_node))
		{
			if (sentinel){
				atomic_store(&(f->hash_table[bucket]), (atomic_uintptr_t)new_node);
				return NULL;
			}
			atomic_fetch_add(&(f->entries), 1);
			return fib_node_to_user(f, new_node);
		}
	}
}

void* fib_insert(struct fib *f, const net_addr *a)
{
	int row = reserve_row(f);
	void* r = fib_insert2(f, a, row, 0);
	release_row(f, row);
	return r;
}

/**
 * fib_find - search for FIB node by prefix
 * @f: FIB to search in
 * @n: network address
 *
 * Search for a FIB node corresponding to the given prefix, return
 * a pointer to it or %NULL if no such node exists.
 */
void *
fib_find(struct fib *f, const net_addr *a){
	ASSERT(f->addr_type == a->type);

	int row = reserve_row(f);

	u32 hash = net_hash(a);
	u32 key = reverseBits(hash);
	

	atomic_uintptr_t *curr = &(f->soft_links[row][0]);

	u32 bucket = hash & atomic_load(&(f->hash_mask));
	atomic_store(curr, atomic_load(&(f->hash_table[bucket])));

	if (atomic_load(curr) == 0){
		//No sentinel node
		fib_insert2(f, NULL, row, bucket);
		atomic_store(curr, atomic_load(&(f->hash_table[bucket])));
	}

	while (atomic_load(curr) != 0 && reverseBits(getHash(f, curr)) <= key){
		if (getHash(f, curr) == hash && getSentinel(curr) == 0){
			// Found the node
			release_row(f, row);
			return fib_node_to_user(f, (struct fib_node *)atomic_load(curr));
		}
		atomic_store(curr, getNextAddress(curr));
	}
		
	// Not found
	release_row(f, row);
	return NULL;
}

/**
 * fib_get - find or create a FIB node
 * @f: FIB to work with
 * @n: network address
 *
 * Search for a FIB node corresponding to the given prefix and
 * return a pointer to it. If no such node exists, create it.
 */
void *
fib_get(struct fib *f, const net_addr *a)
{
	return NULL;
}

static inline void *
fib_route_ip4(struct fib *f, net_addr_ip4 *n)
{

	return NULL;
}

static inline void *
fib_route_ip6(struct fib *f, net_addr_ip6 *n)
{

	return NULL;
}

/**
 * fib_route - CIDR routing lookup
 * @f: FIB to search in
 * @n: network address
 *
 * Search for a FIB node with longest prefix matching the given
 * network, that is a node which a CIDR router would use for routing
 * that network.
 */
void *
fib_route(struct fib *f, const net_addr *n)
{
	ASSERT(f->addr_type == n->type);

	net_addr *n0 = alloca(n->length);
	net_copy(n0, n);

	switch (n->type)
	{
	case NET_IP4:
	case NET_VPN4:
	case NET_ROA4:
	case NET_FLOW4:
		return fib_route_ip4(f, (net_addr_ip4 *)n0);

	case NET_IP6:
	case NET_VPN6:
	case NET_ROA6:
	case NET_FLOW6:
		return fib_route_ip6(f, (net_addr_ip6 *)n0);

	default:
		return NULL;
	}
}


/**
 * fib_delete - delete a FIB node
 * @f: FIB to delete from
 * @E: entry to delete
 *
 * This function removes the given entry from the FIB,
 * taking care of all the asynchronous readers by shifting
 * them to the next node in the canonical reading order.
 */
int fib_delete(struct fib *f, void *E){

	if (E == NULL){
		//printf("Error, deleting NULL\n");
		return 0;
	}

	struct fib_node *n = fib_user_to_node(f, E);

	int row = reserve_row(f);

	int isNodeMarked = 0;
    atomic_uintptr_t* curr = &(f->soft_links[row][0]);
    atomic_uintptr_t* succ = &(f->soft_links[row][1]);

	atomic_store(succ, (atomic_uintptr_t) n);
	u32 hash = getHash(f, succ);

	int count = 0;
    
    while (1){
		count++;
		if (count > 10000){
			//("Fail to remove node %u\n", hash);
		}
        u32 bucket = hash & atomic_load(&(f->hash_mask));

		if (bucket >= atomic_load(&(f->hash_size))){
			//printf("Error, bucket %u is out of range\n", bucket);
			return 0;
		}

        if (atomic_load(&(f->hash_table[bucket])) == 0){
            fib_insert2(f, NULL, row, bucket);
        }

        u32 key = reverseBits(hash);

        
        atomic_store(curr, atomic_load(&(f->hash_table[bucket])));

        if (atomic_load(curr) == 0){
            //During resizing, there may be a small window where the bucket is NULL
            continue;
        }

		while (atomic_load(curr) != 0 && reverseBits(getHash(f, curr)) <= key && getNextAddress(curr) != atomic_load(succ)){
			atomic_store(curr, getNextAddress(curr));
		}

		//Not found
		if (atomic_load(curr) == 0 ){
			if (isNodeMarked) continue;
			release_row(f, row);
			return 0;
		}

		//Found and can try to remove
		else if (getNextAddress(curr) == atomic_load(succ)){
			if (!isNodeMarked) isNodeMarked = setFlagTrue(&(((struct fib_node *) (atomic_load(succ)))->next));
			if (!isNodeMarked){
				//Already marked
				release_row(f, row);
				return 0;
			}
			//Try to remove the node
			atomic_uintptr_t expected;
			atomic_store(&expected, atomic_load(succ));

			int result = atomic_compare_exchange_weak(&(((struct fib_node *) atomic_load(curr))->next), &expected, getNextAddress(succ));
			

			if (result){
				//Node was removed, return
				//Go through softlinks and handovers
				atomic_store(&expected, atomic_load(succ));
				atomic_store(succ, 0);
				atomic_store(curr, 0);
				freeNode(f, &expected, row);
				atomic_fetch_sub(&(f->entries), 1);
				release_row(f, row);
				return 1;
			} else {
				//Node was not removed, restart
				//printf("Failed to remove\n");
				continue;
			}


		} //Already passed it
		else {
			if (isNodeMarked) continue;
			release_row(f, row);
			return 0;
		}
        

    }

	
}

/**
 * fib_free - delete a FIB
 * @f: FIB to be deleted
 *
 * This function deletes a FIB -- it frees all memory associated
 * with it and all its entries.
 */
void fib_free(struct fib *f)
{
	
}

void fit_init(struct fib_iterator *i, struct fib *f)
{
	
}

struct fib_node *
fit_get(struct fib *f, struct fib_iterator *i)
{
	return NULL;
}

void fit_put(struct fib_iterator *i, struct fib_node *n)
{
	
}

void fit_put_next(struct fib *f, struct fib_iterator *i, struct fib_node *n, uint hpos)
{
	
}

void fit_put_end(struct fib_iterator *i)
{
	
}

void fit_copy(struct fib *f, struct fib_iterator *dst, struct fib_iterator *src)
{
	
}
