/**
 * @file
 * @brief This file implements the virtual memory and virtual address handling
 * @copyright Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.
 */

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include "virtual_memory.hpp"

namespace {
	/* file constants */
	/** @todo hardcoded start address */
	const char* ARGO_START = (char*) 0x30000000000UL;
	/** @todo hardcoded end address */
	const char* ARGO_END   = (char*) 0x50000000000UL;
	/** @todo hardcoded size */
	const ptrdiff_t ARGO_SIZE = ARGO_END - ARGO_START;

	/** @brief error message string */
	const std::string msg_alloc_fail = "MEMFD - ArgoDSM could not allocate mappable memory";
	/** @brief error message string */
	const std::string msg_mmap_fail = "MEMFD - ArgoDSM failed to map in virtual address space.";
	/** @brief error message string */
	const std::string msg_main_mmap_fail = "MEMFD - ArgoDSM failed to set up virtual memory. Please report a bug.";

	/* file variables */
	/** @brief a file descriptor for backing the virtual address space used by ArgoDSM */
	int fd;
	/** @brief the address at which the virtual address space used by ArgoDSM starts */
	void* start_addr;
	void* shadow_addr;
}

namespace argo {
	namespace virtual_memory {
		void init() {
#ifdef DEBUG
			printf("init memfd start\n");
#endif
			fd = syscall(__NR_memfd_create,"argocache", 0);
			if(ftruncate(fd, ARGO_SIZE)) {
				std::cerr << msg_main_mmap_fail << std::endl;
				/** @todo do something? */
				throw std::system_error(std::make_error_code(static_cast<std::errc>(errno)), msg_main_mmap_fail);
				exit(EXIT_FAILURE);
			}
			// /** @todo check desired range is free */
			 int flags = MAP_ANONYMOUS|MAP_SHARED|MAP_FIXED|MAP_NORESERVE;
			 start_addr = ::mmap((void*)ARGO_START, ARGO_SIZE, PROT_READ|PROT_WRITE, flags, -1, 0);
			 if(start_addr == MAP_FAILED) {
			 	std::cerr << msg_main_mmap_fail << std::endl;
			 	/** @todo do something? */
			 	throw std::system_error(std::make_error_code(static_cast<std::errc>(errno)), msg_main_mmap_fail);
			 	exit(EXIT_FAILURE);
			 }
#ifdef DEBUG
			printf("init memfd done\n");
#endif
		}

		void* start_address() {
			return start_addr;
		}
		void* shadow_address() {
			return shadow_addr;
		}

		unsigned long size() {
			return ARGO_SIZE;
		}

		void* allocate_mappable(unsigned long alignment, unsigned long size) {
			void* p;
			int flags = MAP_ANONYMOUS|MAP_SHARED|MAP_NORESERVE;
			p = ::mmap(NULL, size, PROT_READ|PROT_WRITE, flags, -1, 0);
			if(p == MAP_FAILED) {
			 	std::cerr << msg_main_mmap_fail << std::endl;
			 	/** @todo do something? */
			 	throw std::system_error(std::make_error_code(static_cast<std::errc>(errno)), msg_main_mmap_fail);
			 	exit(EXIT_FAILURE);
			}
			printf("alloc mappable DONE size:%ld\n",size);
			return p;
		}
		unsigned long mapctr = 0;
		void map_memory(void* addr, unsigned long size, unsigned long offset, int prot) {
		  void* p = ::mmap(addr, size, prot, MAP_SHARED|MAP_FIXED, fd, offset);
#ifdef DEBUG
		printf("map memory ctr:%ld addr %ld, size:%ld, offset:%ld, prot:%d\n",mapctr++,addr,size,offset,prot);
#endif
		  if(p == MAP_FAILED) {
		    std::cerr << msg_mmap_fail << " errno " << errno <<  std::endl;
		    throw std::system_error(std::make_error_code(static_cast<std::errc>(errno)), msg_mmap_fail);
		    exit(EXIT_FAILURE);
		  }
		}
	} // namespace virtual_memory
} // namespace argo
