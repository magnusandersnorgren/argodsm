/**
 * @file
 * @brief this is a legacy file from the ArgoDSM prototype
 * @copyright Eta Scale AB. Licensed under the ArgoDSM Open Source License. See the LICENSE file for details.
 * @deprecated this file is legacy and will be removed as soon as possible
 * @warning do not rely on functions from this file
 */

#ifndef argo_swdsm_h
#define argo_swdsm_h argo_swdsm_h



/* Includes */
#include "mpi.h"
#include "argo.h"
#include "data_transfer.hpp"

#include <type_traits>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <signal.h>
#include <malloc.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/mman.h>
#include <random>
#include <atomic>


#ifndef NUM_THREADS
/** @brief Number of maximum local threads in each node */
/**@bug this limits amount of local threads because of pthread barriers being initialized at startup*/
#define NUM_THREADS 128
#endif

/** @brief Hack to avoid warnings when you have unused variables in a function */
#define UNUSED_PARAM(x) (void)(x)

typedef struct my_argo_prefetch_struct
{
		void *ptr;
		unsigned long long size;
		char mode;
		float predicted_start;
		int workerid;
		void *sched_mutex; //passed from starpu
} argo_prefetch_t;

/** @brief Struct for cache control data*/
typedef struct myControlData //global cache control data / directory
{
		char dirty;   //Is this locally dirty?  
		unsigned long long tag;   //addres of global page in distr mem
#ifdef CACHE_TRACE
		unsigned long long usectr;   //addres of global page in distr mem
#endif
} control_data;

/** @brief Struct containing statistics */
typedef struct argo_statisticsStruct
{
		/** @brief Time spend locking */
		double locktime;
		double signallock;
		/** @brief Time spent self invalidating */
		double selfinvtime; 
		/** @brief Time spent loading pages */
		double loadtime;
		/** @brief Time spent storing pages */
		double storetime; 
		/** @brief Time spent writing back from the writebuffer */
		double writebacktime; 
		/** @brief Time spent flushing the writebuffer */
		double flushtime;
		double prefetchwait;

		double loadtransfertime;
		double loadtransferavg;
		double maptime;
		double evicttime; 
		/** @brief Time spent in global barrier */
		double barriertime;
		double searchcachetime; 
		/** @brief Number of stores */
		unsigned long long stores;
		unsigned long long handler_requests; 
		/** @brief Number of loads */
		unsigned long long loads; 
		/** @brief Number of barriers executed */
		unsigned long long barriers; 
		/** @brief Number of writebacks from (full) writebuffer */
		unsigned long long writebacks; 
		/** @brief Number of locks */
		int locks;
		int prefetchesissued;
		int evictions;
} argo_statistics;

/*constants for control values*/
/** @brief Constant for invalid states */
static const char INVALID=0;
/** @brief Constant for valid states */
static const char VALID=1;
/* @brief Constant for clean states */
static const char CLEAN=2;
/** @brief Constant for dirty states */
static const char DIRTY=3;
/** @brief Constant for writer states */
static const char WRITER=4;
/** @brief Constant for reader states */
static const char READER=5;
static const char PREFETCH=6;

/*Handler*/
/**
 * @brief Catches memory accesses to memory not yet cached in ArgoDSM. Launches remote requests for memory not present.
 * @param sig unused param
 * @param si contains information about faulting instruction such as memory address
 * @param unused is a unused param but needs to be declared
 * @see signal.h
 */
void handler(int sig, siginfo_t *si, void *unused);
/**
 * @brief Sets up ArgoDSM's signal handler
 */
void set_sighandler();

/*ArgoDSM init and finish*/
/**
 * @brief Initializes ArgoDSM runtime
 * @param size Size of wanted global address space
 */
void argo_initialize(unsigned long long size);

/**
 * @brief Shutting down ArgoDSM runtime
 */
void argo_finalize();

/*Synchronization*/
/**
 * @brief Self-Invalidates all memory in caches
 */
void self_invalidation();

/**
 * @brief Global barrier for ArgoDSM - needs to be called by every thread in the
 *        system that need coherent view of the memory
 * @param n number of local thread participating
 */
void swdsm_argo_barrier(int n);

/**
 * @brief acquire function for ArgoDSM (Acquire according to Release Consistency)
 */
void argo_acquire();
/**
 * @brief Release function for ArgoDSM (Release according to Release Consistency)
 */
void argo_release();

/**
 * @brief acquire-release function for ArgoDSM (Both acquire and release
 *        according to Release Consistency)
 */
void argo_acq_rel();

/*Reading and Writing pages*/
/**
 * @brief Writes back the whole writebuffer (All dirty data on the node)
 */
void flushWriteBuffer(void);

/*Statistics*/
/**
 * @brief Clears out all statistics
 */
void clear_statistics();

/**
 * @brief wrapper for MPI_Wtime();
 * @return Wall time
 */
double argo_wtime();

/**
 * @brief Prints collected statistics
 */
void print_statistics();

/**
 * @brief Resets current coherence. Collective function called by all threads on all nodes.
 * @param n number of threads currently spawned on each node. 
 */
void argo_reset_coherence(int n);

/**
 * @brief Gives the ArgoDSM node id for the local process
 * @return Returns the ArgoDSM node id for the local process
 */
unsigned int argo_get_nid();

/**
 * @brief Gives number of ArgoDSM nodes
 * @return Number of ArgoDSM nodes
 */
unsigned int argo_get_nodes();

/**
 * @brief Gives a pointer to the global address space
 * @return Start address of the global address space
 */
void *argo_get_global_base();

/**
 * @brief Size of global address space
 * @return Size of global address space
 */
unsigned long long argo_get_global_size();

/**
 * @brief Initializes the MPI environment
 */
void initmpi();

/**
 * @brief Gets cacheindex for a given address
 * @param addr Address in the global address space
 * @param hint can give some idea why we need the index, perhaps we are using it for prefetching and only want indices that have invalid data etc
 * @return cacheindex where addr should map to in the ArgoDSM page cache
 */
long get_cache_index(unsigned long long addr, unsigned long long hint);

/**
 * @brief Returns an address corresponding to the page addr is addressing
 * @param addr Address in the global address space
 * @return addr rounded down to nearest multiple of pagesize
 */
unsigned long long align_addr(unsigned long long addr);

/**
 * @brief Gives homenode for a given address
 * @param addr Address in the global address space
 * @return Process ID of the node backing the memory containing addr
 */
unsigned long long get_homenode(unsigned long long addr);

/**
 * @brief Gets the offset of an address on the local nodes part of the global memory
 * @param addr Address in the global address space
 * @return addr-(start address of local process part of global memory)
 */
unsigned long long get_offset(unsigned long long addr);

/**
 * @brief Gives an index to the sharer/writer vector depending on the address
 * @param addr Address in the global address space
 * @return index for sharer vector for the page
 */
unsigned long long get_classification_index(unsigned long long addr);

int argo_in_cache(void* ptr, unsigned long long size);
void clear_state();
void *starpu_prefetcher(void *x);
double argo_get_taskavg();
double argo_get_signalavg();
int argo_get_sched_policy();
void remove_cache_index(unsigned long long addr, unsigned long long index);
#endif /* argo_swdsm_h */

