#include "test/birdtest.h"
#include "test/bt-utils.h"

#include "nest/route.h"

#include <stdio.h>

//Function to test

void fib_init(struct fib *f, pool *p, uint addr_type, uint node_size, uint node_offset, uint hash_order, fib_init_fn init);
void *fib_find(struct fib *, const net_addr *);	/* Find or return NULL if doesn't exist */
void *fib_get_chain(struct fib *f, const net_addr *a); /* Find first node in linked list from hash table */
void *fib_get(struct fib *, const net_addr *);	/* Find or create new if nonexistent */
void *fib_route(struct fib *, const net_addr *); /* Longest-match routing lookup */
void fib_delete(struct fib *, void *);	/* Remove fib entry */
void fib_free(struct fib *);		/* Destroy the fib */
void fib_check(struct fib *);		/* Consistency check for debugging */


static int
t_fib_simple(void){

    resource_init(); //Initialize the root pool

    struct linpool *lp = lp_new_default(&root_pool);

    struct fib *f = lp_alloc(lp, sizeof(struct fib));

    //Initialize the fib
    fib_init(f, lp, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

    printf("Offset of net_addr_ip4 is %u\n", OFFSETOF(net, n));

    //Add a route
    
    net_addr_ip4 a = NET_ADDR_IP4(2040257024, 24); //ip of 121.155.218.0/24

    //Is a pointer to the fib node with a rte* before it(in memory address)
    net* pointer_to_a = fib_get(f, &a);

    bt_assert_msg(pointer_to_a, "Failed to add node in empty fib\n"); //Check if pointer is not null

    bt_assert_msg(net_equal_ip4(&(pointer_to_a->n.addr), &a), "Node received is not the node added\n");

    pointer_to_a = fib_find(f, &a);

    bt_assert_msg(pointer_to_a, "Failed to find node which was added\n"); //Check if pointer is not null

    bt_assert_msg(net_equal_ip4(&(pointer_to_a->n.addr), &a), "Node found is not the node added\n");

    return 1;
}






int
main(int argc, char *argv[])
{
  bt_init(argc, argv);

  bt_test_suite(t_fib_simple, "Testing Simple operation fib");

  return bt_exit_value();
}