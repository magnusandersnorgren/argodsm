/**
 * @file
 * @brief This file provides a dynamically growing memory pool for ArgoDSM
 * @copyright Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.
 */

#ifndef argo_global_mempool_hpp
#define argo_global_mempool_hpp argo_global_mempool_hpp

/** @todo Documentation */
constexpr int PAGESIZE = 40960;

#include "../backend/backend.hpp"
#include "../synchronization/global_tas_lock.hpp"
#include "../data_distribution/data_distribution.hpp"

#include <sys/mman.h>
#include <memory>
#include <iostream>
#include <stdlib.h>

namespace argo {
	namespace mempools {
		/**
		 * @brief Globalally growing memory pool
		 */
		template<std::size_t chunk_size=409600000>
		class global_memory_pool {
			private:
				/** @brief current base address of this memory pool's memory */
				char* memory;

				/** @brief current size of the memory pool */
				unsigned long max_size;

				/** @brief amount of memory in pool that is already allocated */
				//std::ptrdiff_t* offset;
				unsigned long *offset;
				/** @todo Documentation */
				argo::globallock::global_tas_lock *global_tas_lock;
			public:
				/** type of allocation failures within this memory pool */
				using bad_alloc = std::bad_alloc;

				/** reserved space for internal use */
				unsigned long reserved = 40960;
				/**
				 * @brief Default constructor: initializes memory on heap and sets offset to 0
				 * @param size The amount of memory in the pool
				 */
				global_memory_pool(unsigned long size) {
					//size+=reserved;
					backend::init(size);
					auto nodes = backend::number_of_nodes();
					memory = backend::global_base();
					max_size = backend::global_size();
					if(nodes > 1){
							reserved = max_size/nodes;
					}
					//offset = new (&memory[0]) ptrdiff_t;
					//offset = new (&memory[0]) unsigned long;
					offset = (unsigned long *) malloc(sizeof(unsigned long));
					/**@todo this initialization should move to tools::init() land */
					using namespace data_distribution;
					naive_data_distribution<0>::set_memory_space(nodes, memory, max_size);
					//					bool* flag = new (&memory[sizeof(std::size_t)]) bool;
					bool* flag = new (&memory[sizeof(long)]) bool;
					global_tas_lock = new argo::globallock::global_tas_lock(flag);

					//if(backend::node_id()==0){
					  /**@todo if needed - pad offset to be page or pagecache size and make sure offset and flag fits */
						//					  *offset = static_cast<std::ptrdiff_t>(reserved);
					  *offset = static_cast<long>(reserved);
						//}
					backend::barrier();
				}

				/** @todo Documentation */
				~global_memory_pool(){
					backend::finalize();
					printf("Total memory used during ArgoDSM lifetime: %ld\n", *offset);
					delete global_tas_lock;

				};

				/**
				 *@brief  Resets the memory pool to the initial state instead of de-allocating and (re)allocating all buffers again.
				 *Resets the memory pool to the initial state instead of de-allocating and (re)allocating all buffers again.
				 *Any allocator or memory pool depending on this memory pool now has undefined behaviour.
				 */
				void reset(){
					backend::barrier();
					memory = backend::global_base();
					max_size = backend::global_size();
					//if(backend::node_id()==0){
						/**@todo if needed - pad offset to be page or pagecache size and make sure offset and flag fits */
						//						*offset = static_cast<std::ptrdiff_t>(reserved);
						*offset = static_cast<long>(reserved);
						//}
					backend::barrier();
				}

				/**
				 * @brief Reserve more memory
				 * @param size Amount of memory reserved
				 * @return The pointer to the first byte of the newly reserved memory area
				 * @todo move size check to separate function?
				 */
				char* reserve(unsigned long size, std::size_t alignment=32) {
					char* ptr;

					global_tas_lock->lock();
					if(*offset+size > max_size) {
						global_tas_lock->unlock();
						throw bad_alloc();
					}

					*offset = ((*offset+alignment-1)/alignment)*alignment;
					ptr = &memory[*offset];
					*offset += size;
					global_tas_lock->unlock();
					return ptr;
				}


				/**
				 * @brief fail to grow the memory pool
				 * @param size minimum size to grow
				 */
				void grow(std::size_t size) {
					(void)size; // size is not needed for unconditional failure
					throw std::bad_alloc();
				}

				/**
				 * @brief check remaining available memory in pool
				 * @return remaining bytes in memory pool
				 */
				std::size_t available() {
					std::size_t avail;
					global_tas_lock->lock();
					avail = max_size - *offset;
					global_tas_lock->unlock();
					return avail;
				}
				
				template<typename T>
				bool is_inside(T* ptr) {
					return memory != nullptr && static_cast<char*>(ptr) > memory && static_cast<char*>(ptr) < (memory + max_size);
				}
		};
	} // namespace mempools
} // namespace argo

#endif /* argo_global_mempool_hpp */
