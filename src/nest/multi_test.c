#include "test/birdtest.h"
#include "test/bt-utils.h"
#include "lib/resource.h"

#include "nest/route.h"

#include <stdio.h>

#include <pthread.h>





typedef struct threadArgs{
    struct fib* fib;
    int threadNumber;
} threadArgs;

static int
t_fib_simple(void){

    resource_init(); //Initialize the root pool


    struct fib *f = mb_alloc(&root_pool, sizeof(struct fib));

    //Initialize the fib
    fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

    //printf("Offset of net_addr_ip4 is %u\n", OFFSETOF(net, n)); Result is 8 (a pointer)

    //Add a route
    
    net_addr_ip4 a = NET_ADDR_IP4(2040257024, 24); //ip of 121.155.218.0/24


    //Is a pointer to the fib node with a rte* before it(in memory address)
    net* pointer_to_a = fib_get(f, (net_addr*) &a);

    bt_assert_msg(pointer_to_a != NULL, "Failed to add node in empty fib\n"); //Check if pointer is not null

    net_addr *b = &(pointer_to_a->n.addr[0]);
    net_addr_ip4 *c = (net_addr_ip4*) b;
    
    bt_assert_msg(net_equal(b, (net_addr*) (&a)), "Node received is not the node added\n");

    pointer_to_a = fib_find(f, (net_addr*) &a);


    bt_assert_msg(pointer_to_a!= NULL, "Failed to find node which was added\n"); //Check if pointer is not null

    b = &(pointer_to_a->n.addr[0]);
    c = (net_addr_ip4*) b;

    bt_assert_msg(net_equal(b, (net_addr*) (&a)), "Node found is not the node added\n");

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
        bt_assert_msg(net_equal_ip4((net_addr_ip4*) &(entry->n.addr), &a), "Entry found is not the entry added\n");
        fib_delete(f, entry);
    }

    bt_assert_msg(f->entries == 0, "Fib count is not 0 after removing every entries\n");

    fib_free(f);

    return 1;

}


void* f_multi_Add(void* argus){

    threadArgs* args = (threadArgs*) argus;

    int threadNumber = args->threadNumber;

    for (int i = 0; i < 10000; i++){
        net_addr_ip4 a = NET_ADDR_IP4(6*i + threadNumber, 32);
        net* entry = fib_get(args->fib, (net_addr*) &a);
        bt_assert_msg(entry, "Failed to add node %d in t_fib_10000_address\n", i);
    }
    
    
}

void* f_multi_remove(void* argus){

    threadArgs* args = (threadArgs*) argus;

    int threadNumber = args->threadNumber;

    for (int i = 0; i < 10000; i++){
        net_addr_ip4 a = NET_ADDR_IP4(6*i + threadNumber, 32);
        net* entry = fib_find(args->fib, (net_addr*) &a);
        bt_assert_msg(entry, "Failed to add node %d in t_fib_10000_address\n", i);
        fib_delete(args->fib, entry);
    }
} 





static int t_multi_thread(void){
    resource_init(); //Initialize the root pool

    struct fib *f = mb_alloc(&root_pool, sizeof(struct fib));

    //Initialize the fib
    fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

    pthread_t threads[6];
    threadArgs args[6];

    for (int i = 0; i < 6; i++){
        args[i].fib = f;
        args[i].threadNumber = i;
    }

    //Create threads
    for (int i = 0; i < 6; i++){
        pthread_create(&threads[i], NULL, f_multi_Add, (void*) &args[i]);
    }

    for (int i = 0; i < 6; i++){
        pthread_join(threads[i], NULL);
    }


    //Check if every entry is in the fib

    for (int i = 0; i < 60000; i++){
        net_addr_ip4 a = NET_ADDR_IP4(i, 32);
        net* entry = fib_find(f, (net_addr*) &a);
        bt_assert_msg(entry, "Failed to find node %d in t_fib_10000_address\n", i);
        bt_assert_msg(net_equal_ip4((net_addr_ip4*) &(entry->n.addr), &a), "Entry found is not the entry added\n");
    }

    bt_assert_msg(f->entries == 60000, "Fib count is not 60000\n");


    //Remove every entry

    for (int i = 0; i < 6; i++){
        pthread_create(&threads[i], NULL, f_multi_remove, (void*) &args[i]);
    }

    for (int i = 0; i < 6; i++){
        pthread_join(threads[i], NULL);
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
  bt_test_suite(t_multi_thread, "Testing Adding/remove operation in multithreaded fib");

  return bt_exit_value();
}