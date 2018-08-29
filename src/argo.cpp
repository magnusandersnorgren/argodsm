/**
 * @file
 * @brief This file implements some of the basic ArgoDSM calls
 * @copyright Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.
 */
#include<dlfcn.h>
#include "argo.hpp"

#include "allocators/collective_allocator.hpp"
#include "allocators/dynamic_allocator.hpp"

#include "virtual_memory/virtual_memory.hpp"

namespace vm = argo::virtual_memory;
namespace mem = argo::mempools;
namespace alloc = argo::allocators;

/* some memory pools for default use */
/** @todo should be static? */
mem::global_memory_pool<>* default_global_mempool;
mem::dynamic_memory_pool<alloc::global_allocator, mem::NODE_ZERO_ONLY> collective_prepool(&alloc::default_global_allocator);
mem::dynamic_memory_pool<alloc::global_allocator, mem::ALWAYS> dynamic_prepool(&alloc::default_global_allocator);

static bool argo_is_loaded = false;
static void argo_loaded()  __attribute__((constructor));
void argo_loaded() {
	argo_is_loaded = true;
}
namespace argo {
	void init(size_t size) {
		printf("argo global mempool @ %p\n", default_global_mempool);
		vm::init();
		printf("argo global mempool @ %p\n", default_global_mempool);
		default_global_mempool = new mem::global_memory_pool<>(size);
		printf("argo global mempool @ %p\n", default_global_mempool);
		argo_reset();
	}
	void finalize() {
		delete default_global_mempool;
	}
	int node_id() {
		return static_cast<int>(argo::backend::node_id());
	}
	int number_of_nodes() {
		return static_cast<int>(argo::backend::number_of_nodes());
	}
	void clear_state(){
		argo::backend::clear_state();
	}
	int in_cache(void* ptr, size_t size){
		return argo::backend::in_cache(ptr,size);
	}
	double get_taskavg(){
		return argo::backend::get_taskavg();
	}
	int get_sched_policy(){
		return argo::backend::get_sched_policy();
	}
	double get_signalavg(){
		return argo::backend::get_signalavg();
	}
} // namespace argo

extern "C" {
	void argo_init(size_t size) {
		argo::init(size);
	}

	void argo_finalize() {
	  argo::finalize();
	}

	void argo_reset() {
		default_global_mempool->reset();
		using namespace alloc;
		using namespace mem;
		collective_prepool = dynamic_memory_pool<global_allocator, NODE_ZERO_ONLY>(&default_global_allocator);
		dynamic_prepool = dynamic_memory_pool<global_allocator, ALWAYS>(&default_global_allocator);
		default_global_allocator = global_allocator<char>();
		default_dynamic_allocator = default_dynamic_allocator_t();
		default_collective_allocator = collective_allocator();
		default_global_allocator.set_mempool(default_global_mempool);
		default_dynamic_allocator.set_mempool(&dynamic_prepool);
		default_collective_allocator.set_mempool(&collective_prepool);
	}

	int argo_node_id() {
		return argo::node_id();
	}

	int argo_number_of_nodes() {
		return argo::number_of_nodes();
	}
  int argo_is_ready() {
		//auto handle = dlopen("libnuma.so", RTLD_NOW|RTLD_GLOBAL);
		//handle = dlopen("libpthread.so.0", RTLD_NOW|RTLD_GLOBAL);
		//handle = dlopen("/sw/parallel/openmpi/tintin-sl6/1.8.5gcc5.1.0/lib/libopen-rte.so.7", RTLD_NOW|RTLD_GLOBAL);
		//(void)handle;
		return argo_is_loaded && argo_allocators_is_ready() && argo::backend::is_loaded();
	}
	int argo_in_cache(void* ptr, size_t size){
		return argo::in_cache(ptr,size);
	}
	void argo_clear_state(){
		return argo::clear_state();
	}
	double argo_get_taskavg(){
		return argo::get_taskavg();
	}
	int argo_get_sched_policy(){
		return argo::get_sched_policy();
	}
	double argo_get_signalavg(){
		return argo::get_signalavg();
	}
}
