/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2009-2010, 2012-2015  Université de Bordeaux
 * Copyright (C) 2010, 2011, 2012, 2013  Centre National de la Recherche Scientifique
 *
 * StarPU is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * StarPU is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#include <errno.h>

#include <core/workers.h>
#include <common/config.h>
#include <starpu.h>
#include <drivers/opencl/driver_opencl.h>
#include <datawizard/memory_manager.h>
#include <datawizard/memory_nodes.h>


/* Get argo allocators */
#include "allocators/allocators.h"
#include "argo.h"

static size_t _malloc_align = sizeof(void*);
static int disable_pinning;

void starpu_malloc_set_align(size_t align)
{
	STARPU_ASSERT_MSG(!(align & (align - 1)), "Alignment given to starpu_malloc_set_align (%lu) must be a power of two", (unsigned long) align);
	if (_malloc_align < align)
		_malloc_align = align;
}

#if (defined(STARPU_USE_CUDA) && !defined(HAVE_CUDA_MEMCPY_PEER))// || defined(STARPU_USE_OPENCL)
struct malloc_pinned_codelet_struct
{
	void **ptr;
	size_t dim;
};
#endif

/* Would be difficult to do it this way, we need to remember the cl_mem to be able to free it later... */
#if defined(STARPU_USE_CUDA) && !defined(HAVE_CUDA_MEMCPY_PEER) && !defined(STARPU_SIMGRID)
static void malloc_pinned_cuda_codelet(void *buffers[] STARPU_ATTRIBUTE_UNUSED, void *arg)
{
	struct malloc_pinned_codelet_struct *s = arg;

	cudaError_t cures;
	cures = cudaHostAlloc((void **)(s->ptr), s->dim, cudaHostAllocPortable);
	if (STARPU_UNLIKELY(cures))
		STARPU_CUDA_REPORT_ERROR(cures);
}
#endif

#if (defined(STARPU_USE_CUDA) && !defined(HAVE_CUDA_MEMCPY_PEER)) && !defined(STARPU_SIMGRID)// || defined(STARPU_USE_OPENCL)
static struct starpu_perfmodel malloc_pinned_model =
{
	.type = STARPU_HISTORY_BASED,
	.symbol = "malloc_pinned"
};

static struct starpu_codelet malloc_pinned_cl =
{
	.cuda_funcs = {malloc_pinned_cuda_codelet},
	.nbuffers = 0,
	.model = &malloc_pinned_model
};
#endif

//Argo modified
int starpu_malloc_flags(void **A, size_t dim, int flags)
{
	int ret=0;
	STARPU_ASSERT(A);

#ifdef ARGO_DEBUG
	_STARPU_DISP("starpumallocflags alloc mem ptr:%p, size:%ld node%d/%d\n",*A,dim,argo_node_id(),argo_number_of_nodes());
#endif
	*A = dynamic_alloc(dim,8); //8byte alignment might be ok (Unclear what is good here)
#ifdef ARGO_DEBUG
	_STARPU_DISP("starpumallocflags2 alloc mem ptr:%p, size:%ld node%d/%d\n",*A,dim,argo_node_id(),argo_number_of_nodes());
#endif
	if (!*A)
		ret = -ENOMEM;

	return ret;
}

int starpu_malloc(void **A, size_t dim)
{
	return starpu_malloc_flags(A, dim, STARPU_MALLOC_PINNED);
}

#if defined(STARPU_USE_CUDA) && !defined(HAVE_CUDA_MEMCPY_PEER) && !defined(STARPU_SIMGRID)
static void free_pinned_cuda_codelet(void *buffers[] STARPU_ATTRIBUTE_UNUSED, void *arg)
{
	cudaError_t cures;
	cures = cudaFreeHost(arg);
	if (STARPU_UNLIKELY(cures))
		STARPU_CUDA_REPORT_ERROR(cures);
}
#endif


#if defined(STARPU_USE_CUDA) && !defined(HAVE_CUDA_MEMCPY_PEER) && !defined(STARPU_SIMGRID) // || defined(STARPU_USE_OPENCL)
static struct starpu_perfmodel free_pinned_model =
{
	.type = STARPU_HISTORY_BASED,
	.symbol = "free_pinned"
};

static struct starpu_codelet free_pinned_cl =
{
	.cuda_funcs = {free_pinned_cuda_codelet},
	.nbuffers = 0,
	.model = &free_pinned_model
};
#endif

//Argo modified
int starpu_free_flags(void *A, size_t dim, int flags)
{
	//Very simplified - we just use the argo memory
	dynamic_free(A); //Errors will be catched in argos dynamic_free
	return 0; //Starpu free flags also always returned 0
}

int starpu_free(void *A)
{
	return starpu_free_flags(A, 0, STARPU_MALLOC_PINNED);
}

#ifdef STARPU_SIMGRID
static starpu_pthread_mutex_t cuda_alloc_mutex = STARPU_PTHREAD_MUTEX_INITIALIZER;
static starpu_pthread_mutex_t opencl_alloc_mutex = STARPU_PTHREAD_MUTEX_INITIALIZER;
#endif

//Argo modified
static uintptr_t
_starpu_malloc_on_node(unsigned dst_node, size_t size)
{
	uintptr_t addr = 0;
	addr = dynamic_alloc(size,8); //May want to play around with the alignment. Perhaps align to a task granularity can be worth it?
	return addr;
}

//Argo modified
void
_starpu_free_on_node(unsigned dst_node, uintptr_t addr, size_t size)
{
	dynamic_free(addr);
	return 0;
}

int
starpu_memory_unpin(void *addr STARPU_ATTRIBUTE_UNUSED, size_t size STARPU_ATTRIBUTE_UNUSED)
{
	if (STARPU_MALLOC_PINNED && disable_pinning <= 0 && STARPU_RUNNING_ON_VALGRIND == 0)
	{
#if defined(STARPU_USE_CUDA) && defined(HAVE_CUDA_MEMCPY_PEER)
		if (cudaHostUnregister(addr) != cudaSuccess)
			return -1;
#endif
	}
	return 0;
}

/*
 * On CUDA which has very expensive malloc, for small sizes, allocate big
 * chunks divided in blocks, and we actually allocate segments of consecutive
 * blocks.
 *
 * We try to keep the list of chunks with increasing occupancy, so we can
 * quickly find free segments to allocate.
 */

/* Size of each chunk, 32MiB granularity brings 128 chunks to be allocated in
 * order to fill a 4GiB GPU. */
#define CHUNK_SIZE (32*1024*1024)

/* Maximum segment size we will allocate in chunks */
#define CHUNK_ALLOC_MAX (CHUNK_SIZE / 8)

/* Granularity of allocation, i.e. block size, StarPU will never allocate less
 * than this.
 * 16KiB (i.e. 64x64 float) granularity eats 2MiB RAM for managing a 4GiB GPU.
 */
#define CHUNK_ALLOC_MIN (16*1024)

/* Number of blocks */
#define CHUNK_NBLOCKS (CHUNK_SIZE/CHUNK_ALLOC_MIN)

/* Linked list for available segments */
struct block {
	int length;	/* Number of consecutive free blocks */
	int next;	/* next free segment */
};

/* One chunk */
LIST_TYPE(_starpu_chunk,
	uintptr_t base;

	/* Available number of blocks, for debugging */
	int available;

	/* Overestimation of the maximum size of available segments in this chunk */
	int available_max;

	/* Bitmap describing availability of the block */
	/* Block 0 is always empty, and is just the head of the free segments list */
	struct block bitmap[CHUNK_NBLOCKS+1];
)

/* One list of chunks per node */
static struct _starpu_chunk_list chunks[STARPU_MAXNODES];
/* Number of completely free chunks */
static int nfreechunks[STARPU_MAXNODES];
/* This protects chunks and nfreechunks */
static starpu_pthread_mutex_t chunk_mutex[STARPU_MAXNODES];

void
_starpu_malloc_init(unsigned dst_node)
{
	_starpu_chunk_list_init(&chunks[dst_node]);
	nfreechunks[dst_node] = 0;
	STARPU_PTHREAD_MUTEX_INIT(&chunk_mutex[dst_node], NULL);
	disable_pinning = starpu_get_env_number("STARPU_DISABLE_PINNING");
}

void
_starpu_malloc_shutdown(unsigned dst_node)
{
	struct _starpu_chunk *chunk, *next_chunk;

	STARPU_PTHREAD_MUTEX_LOCK(&chunk_mutex[dst_node]);
	for (chunk = _starpu_chunk_list_begin(&chunks[dst_node]);
	     chunk != _starpu_chunk_list_end(&chunks[dst_node]);
	     chunk = next_chunk)
	{
		next_chunk = _starpu_chunk_list_next(chunk);
		_starpu_free_on_node(dst_node, chunk->base, CHUNK_SIZE);
		_starpu_chunk_list_erase(&chunks[dst_node], chunk);
		free(chunk);
	}
	STARPU_PTHREAD_MUTEX_UNLOCK(&chunk_mutex[dst_node]);
	STARPU_PTHREAD_MUTEX_DESTROY(&chunk_mutex[dst_node]);
}

/* Create a new chunk */
static struct _starpu_chunk *_starpu_new_chunk(unsigned dst_node)
{
	struct _starpu_chunk *chunk;
	uintptr_t base = _starpu_malloc_on_node(dst_node, CHUNK_SIZE);

	if (!base)
		return NULL;

	/* Create a new chunk */
	chunk = _starpu_chunk_new();
	chunk->base = base;

	/* First block is just a fake block pointing to the free segments list */
	chunk->bitmap[0].length = 0;
	chunk->bitmap[0].next = 1;

	/* At first we have only one big segment for the whole chunk */
	chunk->bitmap[1].length = CHUNK_NBLOCKS;
	chunk->bitmap[1].next = -1;

	chunk->available_max = CHUNK_NBLOCKS;
	chunk->available = CHUNK_NBLOCKS;
	return chunk;
}

//Argo modified
uintptr_t
starpu_malloc_on_node(unsigned dst_node, size_t size)
{
	uintptr_t argoaddr = dynamic_alloc(size,8);
	//	_STARPU_DISP("ALLOC MEM ptr:%p  size:%d node%d/%d\n",argoaddr,size,argo_node_id(),argo_number_of_nodes());
	return argoaddr;
}

//Argo modified
void
starpu_free_on_node(unsigned dst_node, uintptr_t addr, size_t size)
{
	dynamic_free(addr);
	return;
}
