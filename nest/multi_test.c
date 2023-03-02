#include "test/birdtest.h"
#include "test/bt-utils.h"
#include "lib/resource.h"

#include "nest/route.h"

#include <stdio.h>




static int
t_fib_simple(void){

    resource_init(); //Initialize the root pool


    struct fib *f = mb_alloc(&root_pool, sizeof(struct fib));

    //Initialize the fib
    fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

    //printf("Offset of net_addr_ip4 is %u\n", OFFSETOF(net, n)); Result is 8 (a pointer)

    //Add a route
    
    net_addr_ip4 a = NET_ADDR_IP4(2040257024, 24); //ip of 121.155.218.0/24

    printf("Prefix len is %u\n", a.pxlen);
    printf("Len is %u\n", a.length);
    printf("Prefix is %u\n", a.prefix);


    //Is a pointer to the fib node with a rte* before it(in memory address)
    net* pointer_to_a = fib_get(f, (net_addr*) &a);

    if (pointer_to_a == NULL){
        printf("Failed to add node in empty fib\n");
    }

    bt_assert_msg(pointer_to_a != NULL, "Failed to add node in empty fib\n"); //Check if pointer is not null

    printf("Prefix len received is %u\n", pointer_to_a->n.addr[0].pxlen);
    printf("Len received is %u\n", pointer_to_a->n.addr[0].length);
    //printf("Address received is %u\n", pointer_to_a->n.addr[0].addr);
    
    bt_assert_msg(net_equal((pointer_to_a->n.addr[0]), (net_addr*) (&a)) == 0, "Node received is not the node added\n");

    pointer_to_a = fib_find(f, (net_addr*) &a);

    printf("Prefix len found is %u\n", pointer_to_a->n.addr[0].pxlen);
    printf("Len found is %u\n", pointer_to_a->n.addr[0].length);
    //printf("Address found is %u\n", pointer_to_a->n.addr[0].addr);

    
    bt_assert_msg(pointer_to_a!= NULL, "Failed to find node which was added\n"); //Check if pointer is not null


    bt_assert_msg(net_equal(&(pointer_to_a->n.addr[0]), (net_addr*) (&a)) == 0, "Node found is not the node added\n");

    fib_free(f);
    
    return 1;
}


static int t_fib_10000_address(void){
    
    
    resource_init(); //Initialize the root pool

    

    struct fib *f = mb_alloc(&root_pool, sizeof(struct fib));

    //Initialize the fib
    fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

    

    //Add a route

    for (int i = 0; i < 10000; i++){
        net_addr_ip4 a = NET_ADDR_IP4(i, 32);
        net* entry = fib_get(f, (net_addr*) &a);
        bt_assert_msg(entry, "Failed to add node %d in t_fib_10000_address\n", i);
    }

    bt_assert_msg(f->entries == 10000, "Fib count is not 10000\n");

    for (int i = 0; i < 10000; i++){
        net_addr_ip4 a = NET_ADDR_IP4(i, 32);
        net* entry = fib_find(f, (net_addr*) &a);
        bt_assert_msg(entry, "Failed to find node %d in t_fib_10000_address\n", i);
        bt_assert_msg(net_equal_ip4((net_addr_ip4*) &(entry->n.addr), &a) == 0, "Entry found is not the entry added\n");
        fib_delete(f, entry);
    }

    bt_assert_msg(f->entries == 0, "Fib count is not 0 after removing every entries\n");

    fib_free(f);

    return 1;

}





int
main(int argc, char *argv[])
{
  bt_init(argc, argv);

  bt_test_suite(t_fib_simple, "Testing Simple operation fib");
  bt_test_suite(t_fib_10000_address, "Testing Adding/get/remove operation fib");

  return bt_exit_value();
}