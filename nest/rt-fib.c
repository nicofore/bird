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
#include "lib/fib.h"
#include "lib/string.h"

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

/*
 * The FIB rehash values are maintaining FIB count betwee

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

static void removeALink(atomic_uintptr_t *ptr)
{
	if (atomic_load(ptr) == 0)
		return;
	struct fib_node *node = (struct fib_node *)atomic_load(ptr);
	atomic_fetch_sub(&(node->sentinel), 2);
}
static char getNumberOfLink(atomic_uintptr_t *ptr)
{
	struct fib_node *node = (struct fib_node *)atomic_load(ptr);
	return (char)(atomic_load(&(node->sentinel)) >> 1);
}




void fib__free(struct fib *f)
{
	// Liberating all route in the hash table
	atomic_uintptr_t curr;
	atomic_uintptr_t succ;

	atomic_uintptr_t *curr_ptr = &curr;
	atomic_uintptr_t *succ_ptr = &succ;

	atomic_store(curr_ptr, atomic_load(&(f->hash_table[0])));
	while (atomic_load(curr_ptr) != 0)
	{
		atomic_store(succ_ptr, getNextAddress(curr_ptr));
		if (getSentinel(curr_ptr))
		{
			free((void *)atomic_load(curr_ptr));
		}
		else
		{
			free(fib_node_to_user(f, (struct fib_node *)atomic_load(curr_ptr)));
		}
		atomic_store(curr_ptr, atomic_load(succ_ptr));
	}

	//Liberating handovers
	struct node_memory *currHandover = f->handovers;

	while (currHandover != NULL)
	{
		struct node_memory *next = currHandover->next;
		free(currHandover);
		currHandover = next;
	}

	// Free the hash table
	free(atomic_load(&(f->hash_table)));

	free(f->reserved_row);

	free(f);
}




static inline u32 fib_hash(struct fib *f, const net_addr *a);

int getParent(u32 bucket, u32 bucketSize)
{
	u32 parent = bucketSize;
	do
	{
		parent = parent >> 1;
	} while (parent > bucket);
	parent = bucket - parent;
	return parent;
}

uint reserve_row(struct fib *f)
{
	while (1)
	{
		for (int i = 0; i < MAX_THREADS; i++)
		{
			if (atomic_load((&(f->reserved_row[i]))) == 0){
				if (!atomic_exchange(&(f->reserved_row[i]), 1))
					return i;
			}
		}
	}
}

void release_row(struct fib *f, uint row)
{
	atomic_store(&(f->reserved_row[row]), 0);
}

static int setFlagTrue(atomic_uintptr_t *ptr)
{
	atomic_uintptr_t old;
	atomic_store(&old, atomic_fetch_or(ptr, 1));
	return (atomic_load(&old) & 1) == 0;
}

int getFlag(atomic_uintptr_t *ptr)
{
	struct fib_node *node = (struct fib_node *)atomic_load(ptr);
	return (int)(atomic_load(&(node->next)) & 1);
}


//Comes from https://stackoverflow.com/questions/746171/efficient-algorithm-for-bit-reversal-from-msb-lsb-to-lsb-msb-in-c
static const unsigned char BitReverseTable256[] = 
{
  0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0, 
  0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8, 
  0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4, 
  0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC, 
  0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2, 
  0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
  0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6, 
  0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
  0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
  0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9, 
  0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
  0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
  0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3, 
  0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
  0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7, 
  0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF
};


u32 reverseBits(u32 x)
{
	u32 reverseValue;
	unsigned char *c = (char *) &reverseValue;
	unsigned char *p = (char *) &x;
	c[0] = BitReverseTable256[p[3]];
	c[1] = BitReverseTable256[p[2]];
	c[2] = BitReverseTable256[p[1]];
	c[3] = BitReverseTable256[p[0]];
    return reverseValue;
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
	if (atomic_load(&(node->sentinel)) & 1)
		return reverseBits(getHashFromSentinel(f, ptr));
	else
		return net_hash(&(node->addr[0]));
}

static uintptr_t getAddress(atomic_uintptr_t *ptr)
{
	return atomic_load(ptr) & ~1;
}

uintptr_t getNextAddress(atomic_uintptr_t *ptr)
{
	struct fib_node *node = (struct fib_node *)atomic_load(ptr);
	return getAddress(&(node->next));
}

char getSentinel(atomic_uintptr_t *ptr)
{
	struct fib_node *node = (struct fib_node *)atomic_load(ptr);
	return (char)(atomic_load(&(node->sentinel)) & 1);
}



// Store in sentinel, lowest bit is flag, rest is number of links

static void addALink(atomic_uintptr_t *ptr)
{
	if (atomic_load(ptr) == 0)
		return;
	struct fib_node *node = (struct fib_node *)atomic_load(ptr);
	atomic_fetch_add(&(node->sentinel), 2);
}



void printfib(struct fib *f)
{
	atomic_uintptr_t ptr = f->hash_table[0];

	while (atomic_load(&ptr) != 0)
	{
		struct fib_node *node = (struct fib_node *)atomic_load(&ptr);
		if (node->sentinel & 1)
			printf("\nSentinel node: ");
		printf("%u ", getHash(f, &ptr));
		atomic_store(&ptr, getNextAddress(&ptr));
	}
	printf("\n");
}

static void freeNode(struct fib *f, atomic_uintptr_t *ptr, int row)
{
	//Add node to handovers
	u8 succ = 0;
	struct node_memory *new_node = malloc(sizeof(struct node_memory));
	struct node_memory *expected;
	new_node->prev = NULL;
	new_node->node = (struct fib_node *)atomic_load(ptr);
	while (!succ){
		new_node->next = f->handovers;
		expected = new_node->next;
		succ = atomic_compare_exchange_strong(&(f->handovers), &(expected), new_node);
	}
	expected->prev = new_node;

}


void * freeHandovers(void* f)
{
	struct fib *fib = (struct fib *)f;
	while (!fib->stopThread){
		struct node_memory *node = fib->handovers_end->prev;
		while (node != NULL){
			struct fib_node* fnode = node->node;
			//Check if can be freed
			if (getNumberOfLink((atomic_uintptr_t*) fnode) == 0){
				//Check in soft_links if can be free

				for (int i = 0; i < MAX_THREADS; i++)
				{
					for (int j = 0; j < 2; j++){
						atomic_uintptr_t ptr = fib->soft_links[i][j];
						if (ptr == (atomic_uintptr_t)fnode){
							//Node still used
							goto next;
						}
						while (getFlag(&ptr)){
							ptr = getNextAddress(&ptr);
							if (ptr == (atomic_uintptr_t)fnode){
								//Node still used
								goto next;
							}
						}
					}
				}
				//Can be freed
				//First node of handovers
				u8 success = 0;
				while (node->prev == NULL){
					struct memory_node *expected = node;
					success = atomic_compare_exchange_strong(&(fib->handovers), &expected, node->next);
					if (success)
						break;
				}
				if (!success && node->prev != NULL){
					//Not first node
					node->prev->next = node->next;
				}
				node->next->prev = node->prev;

				//Can free node
				removeALink(getNextAddress(fnode));
				free(fib_node_to_user(fib, fnode));
				struct node_memory *temp = node;
				node = node->prev;
				free(temp);
				continue;
			}
			next:
			node = node->prev;
		}

		sleep(30);
	}

	fib__free(fib);
	return NULL;
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
	// f->fib_slab = addr_length ? sl_new(p, node_size + addr_length) : NULL;
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

	// atomic_store(&(f->hash_table), mb_alloc(f->fib_pool, atomic_load(&(f->hash_size)) * sizeof(atomic_uintptr_t)));
	atomic_store(&(f->hash_table), malloc(atomic_load(&(f->hash_size)) * sizeof(atomic_uintptr_t)));
	// Put to zero
	for (uint i = 0; i < atomic_load(&(f->hash_size)); i++)
		atomic_store(&(f->hash_table[i]), 0);

	// Allocate the reserved row
	// atomic_store(&(f->reserved_row), mb_alloc(f->fib_pool, sizeof(atomic_bool) * MAX_THREADS));
	atomic_store(&(f->reserved_row), malloc(sizeof(atomic_bool) * MAX_THREADS));
	for (uint i = 0; i < MAX_THREADS; i++)
		atomic_store(&(f->reserved_row[i]), 0);

	// Allocate the softlinks
	atomic_store(&(f->soft_links), malloc(sizeof(atomic_uintptr_t *) * MAX_THREADS));
	for (uint i = 0; i < MAX_THREADS; i++)
	{
		atomic_store(&(f->soft_links[i]), malloc(2 * sizeof(atomic_uintptr_t)));

		atomic_store(&(f->soft_links[i][0]), 0);
		atomic_store(&(f->soft_links[i][1]), 0);
	}

	atomic_store(&(f->entries), 0);
	atomic_store(&(f->entries_min), 0);
	f->init = init;

	atomic_store(&(f->resizing), 0);

	// Adding first node
	// struct fib_node *b = mb_alloc(f->fib_pool, sizeof(struct fib_node));
	struct fib_node *b = malloc(sizeof(struct fib_node));
	atomic_store(&(b->next), 0);
	atomic_store(&(b->sentinel), 1);
	atomic_store(&(f->hash_table[0]), (atomic_uintptr_t)b);

	f->handovers_end = malloc(sizeof(struct node_memory));
	f->handovers_end->next = NULL;
	f->handovers_end->prev = NULL;
	f->handovers_end->node = NULL;
	f->handovers = f->handovers_end;
	f->stopThread = 0;

	pthread_create(&(f->t), NULL, freeHandovers, f);



}

// Resize
static void
fib_rehash(struct fib *f)
{
	// Resize

	if (!atomic_exchange(&(f->resizing), 1))
	{
		atomic_uintptr_t *newBuckets = (atomic_uintptr_t *)malloc(sizeof(atomic_uintptr_t) * 2 * atomic_load(&(f->hash_size)));
		if (newBuckets == NULL)
		{
			exit(1);
		}
		atomic_uintptr_t *temp;
		for (uint i = atomic_load(&(f->hash_size)); i < 2 * atomic_load(&(f->hash_size)); i++)
		{
			atomic_store(&(newBuckets[i]), 0);
		}
		for (uint i = 0; i < atomic_load(&(f->hash_size)); i++)
		{
			atomic_store(&(newBuckets[i]), atomic_load(&(f->hash_table[i])));
		}
		temp = f->hash_table;
		f->hash_table = newBuckets;
		for (uint i = 0; i < atomic_load(&(f->hash_size)); i++)
		{
			if (atomic_load(&(temp[i])) != 0)
				atomic_store(&(newBuckets[i]), atomic_load(&(temp[i])));
		}
		atomic_store(&(f->hash_size), atomic_load(&(f->hash_size)) << 1);
		atomic_store(&(f->hash_mask), (atomic_load(&(f->hash_mask)) << 1) | 1);
		atomic_store(&(f->entries_max), atomic_load(&(f->entries_max)) << 1);
		atomic_fetch_sub(&(f->hash_shift), 1);
		atomic_fetch_add(&(f->hash_order), 1);

		atomic_store(&(f->resizing), 0);
		free(temp);
	}
}

#define CAST(t) (const net_addr_##t *)
#define CAST2(t) (net_addr_##t *)



static inline u32
fib_hash(struct fib *f, const net_addr *a)
{
	/* Same as FIB_HASH() */
	return reverseBits(net_hash(a)) & atomic_load(&(f->hash_mask));
}

static void *
fib_insert2(struct fib *f, int row, u32 bucket)
{

	atomic_uintptr_t *curr = &(f->soft_links[row][0]);
	atomic_uintptr_t *succ = &(f->soft_links[row][1]);


	u32 hash;
	hash = bucket;


	u32 starting_bucket;
	u32 key = reverseBits(hash);
	struct fib_node *new_node = NULL;
	atomic_uintptr_t expected;

	start:
	while (1)
	{

		if (atomic_load(&(f->entries)) >= atomic_load(&(f->entries_max)))
		{
			fib_rehash(f);
		}

		// Checking in which bucket to insert

		starting_bucket = getParent(hash, atomic_load(&(f->hash_size)));


		//If the starting bucket is empty, insert the sentinel node
		if (atomic_load(&(f->hash_table[starting_bucket])) == 0)
		{
			fib_insert2(f, row, starting_bucket);
		}

		

		atomic_store(curr, atomic_load(&(f->hash_table[starting_bucket])));

		if (atomic_load(curr) == 0)
		{
			//Possible during a resize, the hashtable will be updated soon
			continue;
		}

		atomic_store(succ, getNextAddress(curr));

		// Find the right place to insert the node
		while (atomic_load(succ) != 0 && getHash(f, succ) < key)
		{
			atomic_store(curr, getNextAddress(curr));
			if (atomic_load(curr) == 0)
			{
				// Can happen if delete last node, restart
				goto start;
			}
			atomic_store(succ, getNextAddress(curr));
		}


		// If node is a sentinel, succ is the sentinel or a node with the same value
	
		//Already exists
		if (atomic_load(succ) != 0 && getHash(f, succ) == key && getSentinel(succ)) {
			if (new_node != NULL)
			{
				free(new_node);
				new_node = NULL;
			}
			atomic_store(curr, 0);
			atomic_store(succ, 0);
			return NULL;
		}

		
		//Else should be in situation where succ has the same hash or a higher hash (where we want to insert)

		//Need to follow these condition to insert, else restart
		//printf("Key curr is %u, wanting to insert %u\n", reverseBits(getHash(f, curr)), key);
		
		if (atomic_load(curr) != 0 && getHash(f, curr) < key && (atomic_load(succ) == 0 || (atomic_load(succ) != 0 && getHash(f, succ) >= key))) {
			
			if (new_node == NULL)
			{
				new_node = malloc(sizeof(struct fib_node));
			}

			atomic_store(&(new_node->sentinel), 1);
		
			atomic_store(&(new_node->next), atomic_load(succ));
			atomic_store(&expected, atomic_load(succ));

			//printf("Curr is %lu, succ is %lu, getNextAddress %lu, expected is %lu, next is %lu\n", atomic_load(curr), atomic_load(succ), getNextAddress(curr), atomic_load(&expected), atomic_load(&(( (struct fib_node *) atomic_load(curr) )->next)));

			if (atomic_compare_exchange_strong(&(( (struct fib_node *) atomic_load(curr) )->next), &expected, (atomic_uintptr_t) new_node))
			{
				atomic_store(&(f->hash_table[bucket]), (atomic_uintptr_t)new_node);
				atomic_store(curr, 0);
				atomic_store(succ, 0);
				return NULL;
			}
			
			
		}

		//Failed to insert or conditions not met, restart
		goto start;
		
	}
}

struct fib_node *
fib_get_chain(struct fib *f, const net_addr *a, uint row)
{
	ASSERT(f->addr_type == a->type);
	struct fib_node* e = NULL;
	while (!e)
	{
		e = (struct fib_node *)atomic_load(&(f->hash_table[fib_hash(f, a)]));
		if (!e)
			fib_insert2(f, row, fib_hash(f, a));
	}
	e = (struct fib_node*) getNextAddress((atomic_uintptr_t *)&e);

	return e;
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
fib_find(struct fib *f, const net_addr *a)
{
	ASSERT(f->addr_type == a->type);

	int row = reserve_row(f);

	u32 hash = reverseBits(net_hash(a));
	u32 key = net_hash(a);

	atomic_uintptr_t *curr = &(f->soft_links[row][0]);

	start:
	while (1){
		u32 bucket = hash & atomic_load(&(f->hash_mask));
		atomic_store(curr, atomic_load(&(f->hash_table[bucket])));

		if (atomic_load(curr) == 0)
		{
			// No sentinel node
			fib_insert2(f, row, bucket);
			atomic_store(curr, atomic_load(&(f->hash_table[bucket])));
		}

		if (atomic_load(curr) == 0)
		{
			//Can happen during resize
			//printf("No sentinel node\n");
			continue;
		}


		while (atomic_load(curr) != 0 && getHash(f, curr) <= key)
		{
			if (getHash(f, curr) == key && getSentinel(curr) == 0)
			{
				// Check if same address
				struct fib_node *node = (struct fib_node *)atomic_load(curr);
				if (net_equal(a, (&((node)->addr[0])) ) )
				{
					if (getFlag(curr)){
						atomic_store(curr, 0);
						release_row(f, row);
						goto start;
					}
					// Found the node
					release_row(f, row);
					return fib_node_to_user(f, node);
				}
			}
			atomic_store(curr, getNextAddress(curr));
		}

	// Not found
	atomic_store(curr, 0);
	release_row(f, row);
	return NULL;
	}

	
}



void *
fib_get2(struct fib *f, const net_addr *a, int row)
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

	u32 hash = reverseBits(net_hash(a));
	u32 starting_bucket;
	u32 key = net_hash(a);
	struct fib_node *new_node = NULL;
	atomic_uintptr_t expected;

	//printf("Trying to add node with hash %u\n", hash);

	start:
	while (1)
	{

		if (atomic_load(&(f->entries)) >= atomic_load(&(f->entries_max)))
		{
			fib_rehash(f);
		}

		// Checking in which bucket to insert
		starting_bucket = hash & atomic_load(&(f->hash_mask));


		//If bucket has no sentinel nodes, insert one
		if (atomic_load(&(f->hash_table[starting_bucket])) == 0)
		{
			fib_insert2(f, row, starting_bucket);
		}

		

		atomic_store(curr, atomic_load(&(f->hash_table[starting_bucket])));

		if (atomic_load(curr) == 0)
		{
			// Need to wait for resize to end
			//printf("Waiting for resize\n");
			continue;
		}

		atomic_store(succ, getNextAddress(curr));

		//Find the right place to insert the node
		while (atomic_load(succ) != 0 && getHash(f, succ) < key)
		{
			atomic_store(curr, getNextAddress(curr));
			if (atomic_load(curr) == 0)
			{
				// Can happen if delete last node, restart
				//printf("Curr is null\n");
				goto start;
			}
			atomic_store(succ, getNextAddress(curr));
		}


		//Can skip a node with the same hash if node deleted in front of curr
		if (getHash(f, curr) == key && !getSentinel(curr)){
			goto start;
		}

		//Since there could be multiple nodes with same hashes, we need to keep advancing until we find the right node or reach the end of same hashes
		while ((atomic_load(succ) != 0 && getHash(f, succ) <= key))
		{
			if (getHash(f, succ) == key && !getSentinel(succ))
			{
				// Check if same address
				struct fib_node *node = (struct fib_node *)atomic_load(succ);
				if (net_equal(a, (& (node->addr[0]) ) ))
				{
					if (new_node != NULL)
					{
						free(fib_node_to_user(f, new_node));
						new_node = NULL;
					}
					// Found the node
					atomic_store(curr, 0);
					void * r = atomic_load(succ);
					if (getFlag(succ)){
						atomic_store(succ, 0);
						goto start;
					}
					atomic_store(succ, 0);
					
					return (void *) (((uintptr_t) fib_node_to_user(f, r)) | 1);
				}
			}

			atomic_store(curr, getNextAddress(curr));
			if (atomic_load(curr) == 0 || (!getSentinel(curr) && net_equal(a, (& ( ((struct fib_node*) (atomic_load(curr)))->addr[0]) ))))
			{
				
				// Can happen if delete last node, restart
				goto start;
			}

			atomic_store(succ, getNextAddress(curr));
		}


		//Last check for inserting
		if (atomic_load(curr) != 0 && getHash(f, curr) <= key && (atomic_load(succ) == 0 || (atomic_load(succ) != 0 && getHash(f, succ) > key)))
		{
			if (new_node == NULL)
			{
				new_node = calloc(a->length + f->node_size, 1);
				new_node = fib_user_to_node(f, new_node);
				net_copy(&(new_node->addr[0]), a);
			}
			atomic_store(&(new_node->sentinel), 0);
			
			atomic_store(&(new_node->next), atomic_load(succ));
			atomic_store(&expected, atomic_load(succ));

			
			if (atomic_compare_exchange_strong(&(((struct fib_node *)atomic_load(curr))->next), &expected, (atomic_uintptr_t)new_node))
			{
				if (f->init)
					f->init(fib_node_to_user(f, new_node));
				addALink((atomic_uintptr_t *)&new_node);
				atomic_fetch_add(&(f->entries), 1);
				atomic_store(curr, 0);
				atomic_store(succ, 0);
				return fib_node_to_user(f, new_node);
			}
		}	
	}
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
	int row = reserve_row(f);
	void *r = fib_get2(f, a, row);
	release_row(f, row);
	return (void*) ((uintptr_t) r & ~1);
}

static inline void *
fib_route_ip4(struct fib *f, net_addr_ip4 *n)
{
	void *r;

  while (!(r = fib_find(f, (net_addr *) n)) && (n->pxlen > 0))
  {
    n->pxlen--;
    ip4_clrbit(&n->prefix, n->pxlen);
  }

  return r;
}

static inline void *
fib_route_ip6(struct fib *f, net_addr_ip6 *n)
{
  void *r;

  while (!(r = fib_find(f, (net_addr *) n)) && (n->pxlen > 0))
  {
    n->pxlen--;
    ip6_clrbit(&n->prefix, n->pxlen);
  }

  return r;
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
int fib_delete(struct fib *f, void *E)
{

	if (E == NULL)
	{
		return 0;
	}

	struct fib_node *n = fib_user_to_node(f, E);

	uint row = reserve_row(f);

	int isNodeMarked = setFlagTrue(&(n->next));
	
	//Is already marked
	if (!isNodeMarked)
	{
		release_row(f, row);
		return 0;
	}

	atomic_uintptr_t *curr = &(f->soft_links[row][0]);
	atomic_uintptr_t *succ = &(f->soft_links[row][1]);

	atomic_store(succ, (atomic_uintptr_t)n);
	u32 key = getHash(f, succ);
	u32 hash = reverseBits(key);
	

	while (1)
	{
		u32 bucket = hash & atomic_load(&(f->hash_mask));


		if (atomic_load(&(f->hash_table[bucket])) == 0)
		{
			fib_insert2(f, row, bucket);
		}


		atomic_store(curr, atomic_load(&(f->hash_table[bucket])));

		if (atomic_load(curr) == 0)
		{
			// During resizing, there may be a small window where the bucket is NULL
			continue;
		}

		advance:
		while (atomic_load(curr) != 0 && getHash(f, curr) <= key && getNextAddress(curr) != atomic_load(succ))
		{
			atomic_store(curr, getNextAddress(curr));
		}

		

		// Found and can try to remove
		if (atomic_load(curr) != 0 && getNextAddress(curr) == atomic_load(succ))
		{
			atomic_uintptr_t expected;
			atomic_store(&expected, atomic_load(succ));

			int result = atomic_compare_exchange_strong(&(((struct fib_node *)atomic_load(curr))->next), &expected, getNextAddress(succ));

			if (result)
			{
				// Node was removed, return
				// Go through softlinks and handovers
				removeALink(succ);
				if (atomic_load(succ) != 0){
					atomic_store(curr, getNextAddress(succ));
					addALink(curr);
				}
				atomic_store(&expected, atomic_load(succ));
				atomic_store(succ, 0);
				atomic_store(curr, 0);
				freeNode(f, &expected, row);
				atomic_fetch_sub(&(f->entries), 1);
				release_row(f, row);
				return 1;
			}
			else
			{
				// Node was not removed, restart
				continue;
			}

		} // Not found, should not happen if node is in the FIB
		else
		{
			bug("fib_delete() called for invalid node");
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
void fib_free(struct fib *f){
	f->stopThread = 1;
}




void fit_init(struct fib_iterator *i, struct fib *f)
{
	i->row = reserve_row(f);
	i->curr = &(f->soft_links[i->row][0]);
	atomic_store(i->curr, atomic_load(&(f->hash_table[0])));
}

struct fib_node *
fit_get(struct fib *f, struct fib_iterator *i)
{
	// Do nothing with new iterator
	return NULL;
}

void fit_put(struct fib_iterator *i, struct fib_node *n)
{
	// Do nothing with new iterator
}

void fit_put_next(struct fib *f, struct fib_iterator *i, struct fib_node *n, uint hpos)
{
	atomic_uintptr_t add = (atomic_uintptr_t) n;
	atomic_store(i->curr, getNextAddress(&add));
}

void fit_put_end(struct fib_iterator *i)
{	
	atomic_store((i->curr), 0);
}

void fit_copy(struct fib *f, struct fib_iterator *dst, struct fib_iterator *src)
{
	//Copy the curr pointer
	atomic_store(dst->curr, atomic_load(src->curr));
}




void consistency_check(struct fib *f)
{
	int row = reserve_row(f);

	atomic_uintptr_t *curr = &(f->soft_links[row][0]);

	atomic_store(curr, atomic_load(&(f->hash_table[0])));

	u32 currKey;
	char currSentinel;
	u32 counter = 0;

	while (atomic_load(curr) != 0)
	{
		currKey = getHash(f, curr);
		currSentinel = getSentinel(curr);

		atomic_store(curr, getNextAddress(curr));

		if (atomic_load(curr) != 0){
			if (currKey >= getHash(f, curr)){
				if (currKey == getHash(f, curr)){
					if (currSentinel && !getSentinel(curr)){
					}
					else {
						//Problem
						printf("Problem : curr : %u %u, next is %u %u\n", currKey, currSentinel, getHash(f, curr), getSentinel(curr));
					}
				}
				else {
					//Problem
					printf("Key not in order : curr : %u %u, next is %u %u\n", currKey, currSentinel, getHash(f, curr), getSentinel(curr));
					
				}
			}

			if (!getSentinel(curr)){
				counter++;
			}
				
		}
	}

	if (atomic_load(&(f->entries)) != counter){
		printf("Problem : %u entries in the FIB, but %u in the hash table\n", atomic_load(&(f->entries)), counter);
	}

	// Not found
	release_row(f, row);
}


void print_entry_by_key(struct fib *f){
  	uint row = reserve_row(f);					
	atomic_uintptr_t *curr = &(f->soft_links[row][0]); 
	atomic_store(curr, atomic_load(&(f->hash_table[0])));	
	atomic_store(curr, getNextAddress(curr));
	long entry = 0;
	long counter = 0;					
	while (atomic_load(curr)){					
		if (getSentinel(curr)){
			printf("%ld %ld\n", entry, counter);
			entry++;
			counter = 0;
			atomic_store(curr, getNextAddress(curr));
			continue;
		}
		counter++;
		atomic_store(curr, getNextAddress(curr));
	}
	printf("%ld %ld\n", entry, counter);
	release_row(f, row); 
}