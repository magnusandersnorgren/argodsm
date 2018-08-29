/**
 * @file
 * @brief This file provides facilities for handling virtual memory and virtual address space
 * @copyright Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.
 */

#ifndef argo_virtual_memory_virtual_memory_hpp
#define argo_virtual_memory_virtual_memory_hpp argo_virtual_memory_virtual_memory_hpp

#include <utility>

namespace argo {
	namespace virtual_memory {
		/**
		 * @brief initialize the ArgoDSM virtual address space
		 * @todo virtual address space handling should be wrapped into an object
		 */
		void init();

		/**
		 * @brief get a pointer to the ArgoDSM virtual memory
		 * @return the pointer to ArgoDSM virtual memory
		 */
		void* start_address();

		void* shadow_address();
		/**
		 * @brief get size of the ArgoDSM virtual memory
		 * @return the size of the ArgoDSM virtual memory
		 */
		unsigned long size();

		/**
		 * @brief allocate memory that can be mapped into ArgoDSM virtual address space later
		 * @param alignment the alignment of the allocation
		 * @param size size of the allocation
		 * @return a pointer to the new memory allocation
		 * @details this will allocate memory that is guaranteed to work with map_memory().
		 *          Any memory allocated through other means may not be possible to map
		 *          into the visible ArgoDSM virtual memory space later.
		 */
		void* allocate_mappable(unsigned long alignment, unsigned long size);

		/**
		 * @brief map memory into ArgoDSM virtual address space
		 * @param addr the address to map to
		 * @param size the size of the mapping
		 * @param offset the offset into the backing memory
		 * @param prot protection flags for the mapping
		 */
		void map_memory(void* addr, unsigned long size, unsigned long offset, int prot);
	} // namespace virtual_memory
} // namespace argo

#endif
