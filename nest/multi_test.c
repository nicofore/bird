#include "test/birdtest.h"
#include "test/bt-utils.h"
#include "lib/resource.h"

#include "nest/route.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>



typedef struct threadArgs
{
	struct fib *fib;
	int threadNumber;
	atomic_uint *counter;
} threadArgs;

static int
t_fib_simple(void)
{

	resource_init(); // Initialize the root pool

	struct fib *f = malloc(sizeof(struct fib));

	// Initialize the fib
	fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);


	// Add a route

	net_addr_ip4 a = NET_ADDR_IP4(2040257024, 24); // ip of 121.155.218.0/24

	// Is a pointer to the fib node with a rte* before it(in memory address)
	net *pointer_to_a_o = fib_get(f, (net_addr *)&a);


	net_addr *b = &(pointer_to_a_o->n.addr[0]);
	net_addr_ip4 *c = (net_addr_ip4 *)b;

	bt_assert_msg(net_equal(b, (net_addr *)(&a)), "Node received is not the node added\n");



	net* pointer_to_a = fib_find(f, (net_addr *)&a);

	bt_assert_msg(pointer_to_a == pointer_to_a_o, "Failed to find node which was added\n");

	b = &(pointer_to_a->n.addr[0]);
	c = (net_addr_ip4 *)b;

	bt_assert_msg(net_equal(b, (net_addr *)(&a)), "Node found is not the node added\n");



	pointer_to_a = fib_get(f, (net_addr *)&a);

	bt_assert_msg(pointer_to_a == pointer_to_a_o, "Fib_get didn't create a new node\n");



	bt_assert_msg(atomic_load(&(f->entries)) == 1, "Fib_get created a new node\n");


	fib_free(f);

	return 1;
}

static int t_fib_10000_address(void)
{

	resource_init(); // Initialize the root pool

	struct fib *f = malloc(sizeof(struct fib));

	// Initialize the fib
	fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

	// Add a route

	for (int i = 0; i < 10000; i++)
	{
		net_addr_ip4 a = NET_ADDR_IP4(i, 32);
		net *entry = fib_get(f, (net_addr *)&a);
		bt_assert_msg(entry, "Failed to add node %d in t_fib_10000_address\n", i);
	}

	bt_assert_msg(atomic_load(&(f->entries)) == 10000, "Fib count is not 10000\n");
    
	//consistency_check(f);

	for (int i = 0; i < 10000; i++)
	{
		net_addr_ip4 a = NET_ADDR_IP4(i, 32);
		net *entry = fib_find(f, (net_addr *)&a);
		struct fib_node *node = fib_user_to_node(f, entry);
		bt_assert_msg(entry, "Failed to find node %d in t_fib_10000_address\n", i);

		bt_assert_msg(net_equal_ip4((net_addr_ip4 *)&(entry->n.addr), &a), "Entry found is not the entry added\n");

		fib_delete(f, entry);
	}

	bt_assert_msg(f->entries == 0, "Fib count is not 0 after removing every entries\n");


	fib_free(f);

	return 1;
}



void *f_multi_Add(void *argus)
{

	threadArgs *args = (threadArgs *)argus;

	int threadNumber = args->threadNumber;

	for (int i = 0; i < 10000; i++)
	{
		net_addr_ip4 a = NET_ADDR_IP4(6 * i + threadNumber, 32);
		net *entry = fib_get(args->fib, (net_addr *)&a);
		bt_assert_msg(entry, "Failed to add node %d in t_fib_10000_address\n", i);
	}
	return NULL;
}

void *f_multi_remove(void *argus)
{

	threadArgs *args = (threadArgs *)argus;

	int threadNumber = args->threadNumber;

	for (int i = 0; i < 10000; i++)
	{
		net_addr_ip4 a = NET_ADDR_IP4(6 * i + threadNumber, 32);
		net *entry = fib_find(args->fib, (net_addr *)&a);
		bt_assert_msg(entry, "Failed to find %d\n", 6 * i + threadNumber, 32);
		if (!entry)
		{
			printf("Failed to find %d\n", 6 * i + threadNumber);
		}
        fib_delete(args->fib, entry);	
	}
	return NULL;
}

static int t_multi_get_different(void)
{
	resource_init(); // Initialize the root pool

	struct fib *f = malloc(sizeof(struct fib));

	// Initialize the fib
	fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

	pthread_t threads[6];
	threadArgs args[6];

	for (int i = 0; i < 6; i++)
	{
		args[i].fib = f;
		args[i].threadNumber = i;
	}

	// Create threads
	for (int i = 0; i < 6; i++)
	{
		pthread_create(&threads[i], NULL, f_multi_Add, (void *)&args[i]);
	}

	for (int i = 0; i < 6; i++)
	{
		pthread_join(threads[i], NULL);
	}

	//consistency_check(f);

	// Check if every entry is in the fib

	for (int i = 0; i < 60000; i++)
	{
		net_addr_ip4 a = NET_ADDR_IP4(i, 32);
		net *entry = fib_find(f, (net_addr *)&a);
		bt_assert_msg(entry, "Failed to find node %d in t_fib_10000_address\n", i);
		if (entry == NULL)
			printf("Could not find entry %d\n", i);
		bt_assert_msg(net_equal_ip4((net_addr_ip4 *)&(entry->n.addr), &a), "Entry found is not the entry added\n");
		if (!net_equal_ip4((net_addr_ip4 *)&(entry->n.addr), &a))
			printf("Entry found is not the entry added, found pointer %p\n", entry);
	}

	bt_assert_msg(f->entries == 60000, "Fib count is not 60000\n");
	if (f->entries != 60000)
		printf("Fib count is not 60000, found %d\n", f->entries);

	// Remove every entry

	for (int i = 0; i < 6; i++)
	{
		pthread_create(&threads[i], NULL, f_multi_remove, (void *)&args[i]);
	}

	for (int i = 0; i < 6; i++)
	{
		pthread_join(threads[i], NULL);
	}

	bt_assert_msg(f->entries == 0, "Fib count is not 0 after removing every entries\n");

	if (f->entries != 0)
		printf("Fib count is not 0 after removing every entries, found %d\n", f->entries);

	fib_free(f);

	return 1;
}



static int t_single_walk(void){

	resource_init(); // Initialize the root pool
	struct fib *f = malloc(sizeof(struct fib));

	

	// Initialize the fib
	fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

	for (int i = 0; i < 10000; i++){
		net_addr_ip4 a = NET_ADDR_IP4(i, 32);
		net_addr* entry = (net_addr*) &a;
		void * e;
		
		e = fib_get(f, entry);
	}

	int counter = 0;

	net *z;

	FIB_WALK(f, net, z){
		counter++;
	}
	FIB_WALK_END
	bt_assert_msg(counter == 10000, "Did not iterate 10000 but %d\n", counter);


	fib_free(f);
	return 1;
}

static int t_multi_walk(void){

	resource_init(); // Initialize the root pool
	struct fib *f = malloc(sizeof(struct fib));

	

	// Initialize the fib
	fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

	for (int i = 0; i < 100; i++){
		net_addr_ip4 a = NET_ADDR_IP4(i, 32);
		net_addr* entry = (net_addr*) &a;
		void * e;
		
		e = fib_get(f, entry);
	}

	int counter = 0;
	net* z;
	net* z2;

	FIB_WALK(f, net, z){
		FIB_WALK(f, net, z2){
			counter++;
		}
		FIB_WALK_END
	}
	FIB_WALK_END
	bt_assert_msg(counter == 10000, "Did not iterate 10000 but %d\n", counter);


	fib_free(f);
	return 1;
}


static int t_single_ite(void){

	resource_init(); // Initialize the root pool
	struct fib *f = malloc(sizeof(struct fib));

	

	// Initialize the fib
	fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

	for (int i = 0; i < 10000; i++){
		net_addr_ip4 a = NET_ADDR_IP4(i, 32);
		net_addr* entry = (net_addr*) &a;
		void * e;
		
		e = fib_get(f, entry);
	}

	int counter = 0;

	struct fib_iterator* it = malloc(sizeof(struct fib_iterator));

	FIB_ITERATE_INIT(it, f);
	
	FIB_ITERATE_START(f, it, net, z){
		counter++;
	}FIB_ITERATE_END

	bt_assert_msg(counter == 10000, "Did not iterate 10000 but %d\n", counter);

	free(it);

	fib_free(f);
	return 1;
}

static int t_single_ite_put(void){

	resource_init(); // Initialize the root pool
	struct fib *f = malloc(sizeof(struct fib));

	

	// Initialize the fib
	fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

	for (int i = 0; i < 10000; i++){
		net_addr_ip4 a = NET_ADDR_IP4(i, 32);
		net_addr* entry = (net_addr*) &a;
		void * e;
		
		e = fib_get(f, entry);
	}

	int counter = 0;

	struct fib_iterator* it = malloc(sizeof(struct fib_iterator));

	FIB_ITERATE_INIT(it, f);
	
	FIB_ITERATE_START(f, it, net, z){
		counter++;
		FIB_ITERATE_PUT(it);
		counter++;
		counter--;
		FIB_ITERATE_UNLINK(it, f);
	}FIB_ITERATE_END;

	bt_assert_msg(counter == 10000, "Did not iterate 10000 but %d\n", counter);

	free(it);

	fib_free(f);
	return 1;
}


void* f_multi_ite(void* arg){
	threadArgs* args = (threadArgs*) arg;
	int threadNumber = args->threadNumber;
	struct fib *f = args->fib;
	atomic_uint* c = args->counter;
	int counter = 0;

	struct fib_iterator* it = malloc(sizeof(struct fib_iterator));

	FIB_ITERATE_INIT(it, f);

	FIB_ITERATE_START(f, it, net, z){
		if (counter == threadNumber){
			atomic_fetch_add(c, 1);
			while (atomic_load(c) != 0){}
			FIB_ITERATE_PUT_END(it);
		}
		counter++;
	}
	FIB_ITERATE_END;
	free(it);
	return NULL;
}





static int t_multiple_ite(void){

	resource_init(); // Initialize the root pool
	struct fib *f = malloc(sizeof(struct fib));

	

	// Initialize the fib
	fib_init(f, &root_pool, NET_IP4, sizeof(net), OFFSETOF(net, n), 0, NULL);

	atomic_uint c;
	atomic_store(&c, 0);

	for (int i = 0; i < 10; i++){

		for (int i = 0; i < 31; i++){
			net_addr_ip4 a = NET_ADDR_IP4(i, 32);
			net_addr* entry = (net_addr*) &a;
			void * e;
			
			e = fib_get(f, entry);
		}

		threadArgs args[31];
		pthread_t threads[31];

		for (int i = 0; i < 31; i++){
			args[i].fib = f;
			args[i].threadNumber = i;
			args[i].counter = &c;
			pthread_create(&threads[i], NULL, f_multi_ite, &args[i]);
		}

		while(atomic_load(&c) != 31){}

		for (int i = 0; i < 31; i++){
			net_addr_ip4 a = NET_ADDR_IP4(i, 32);
			net* entry = fib_find(args->fib, (net_addr *)&a);
            fib_delete(args->fib, entry);
		}

		atomic_store(&c, 0);

		for (int i = 0; i < 31; i++){
			pthread_join(threads[i], NULL);
		}

	}

	fib_free(f);
	return 1;
}






int main(int argc, char *argv[])
{
	bt_init(argc, argv);
	bt_test_suite(t_fib_simple, "Testing Simple operation fib");
	bt_test_suite(t_fib_10000_address, "Testing Adding/find/remove operation fib");
	bt_test_suite(t_multi_get_different, "Testing Adding/remove operation in multithreaded fib");



	bt_test_suite(t_single_walk, "Testing single walk");
	bt_test_suite(t_multi_walk, "Testing multi walk");
	bt_test_suite(t_single_ite, "Testing single iterator");
	bt_test_suite(t_single_ite_put, "Testing single iterator with put and unlink");

	bt_test_suite(t_multiple_ite, "Testing multiple iterator");
	return bt_exit_value();

}