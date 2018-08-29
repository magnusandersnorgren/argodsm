
/**
 * @file
 * @brief This file implements the MPI-backend of ArgoDSM
 * This part has been developed to particularly be for scaling memory, specifically to tie together with StarPU. May have to be modified to work with normal ArgoDSM, at least to work with the described protocol in HPDC15.
 * @copyright Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.
 */
#include "signal/signal.hpp"
#include "virtual_memory/virtual_memory.hpp"
#include "data_distribution/data_distribution.hpp"
#include "swdsm.h"
#include "data_transfer.hpp"
#include "prefetcher.hpp"
#include <list>
#include <unordered_set>
#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <limits.h>

/*namespaces*/
namespace vm = argo::virtual_memory;
namespace sig = argo::signal;


/*Defines*/
//#define STARPU_SUPPORT_SKIP_INDICES 1
#define STARPU_SUPPORT 1
#define ARGO_BIGMEM_MODE 1

//#define HANDLER_DEBUG 1
//#define DEBUG 1
//#define PREFETCH_DEBUG 1

//If we want to write back diffs - doesnt work with prefetching now
//#define ARGO_DIFF 1
//#define DRF_UNIT 128
//#define ALWAYS_WRITABLE 1

//#define VA_TRACE 0
//#define CACHE_TRACE 1



/*starpu*/
/** @brief  Need to include for accessing starpu task based information */
#ifdef STARPU_SUPPORT
#include "starpu.h"
#include "starpu_task_list.h"
#include "starpu_config.h"
#include "starpu_scheduler.h"
#include "common/fxt.h"
#include "core/task.h"
#include "core/sched_ctx.h"
#include "core/jobs.h"
#include "sched_policies/fifo_queues.h"

struct _starpu_dmda_data
{
		double alpha;
		double beta;
		double _gamma;
		double idle_power;
		struct _starpu_fifo_taskq **queue_array;
		long int total_task_cnt;
		long int ready_task_cnt;
};

#ifdef STARPU_SUPPORT_SKIP_INDICES

/** @brief  Used if we want to try to keep current or future taskdata in the cache, 
		go to a new index cache collision retry times until we find an index without task data cached */
int cache_collision_retry=0; //random repl
#endif

/** @brief  schedule policy for starpu 
		0-normal dmda
		1-if all buffers of a task are cached, put in front of task queue 
		2-if any buffer is  cached put in front of task queue front 
		3-sort based on how much is cached
*/
int sched_policy=0;
/** @brief  schedule policy for starpu 

 // 							 (prefetch_policy==0 && soonest_miss > starpu_timing_now()+taskavg*1000000 && current->argo_cached!=0) ||
 // 							 (prefetch_policy==1 && cachemiss_start+argo_get_signalavg()+cachemiss_avg > MPI_Wtime()+taskavg && current->argo_cached!=0) ||
 // 							 (prefetch_policy==2 && time > starpu_timing_now()+taskavg*1000000 && current->argo_cached!=0 && cachemiss_start+argo_get_signalavg()+cachemiss_avg > MPI_Wtime()+taskavg && soonest_miss > starpu_timing_now()+taskavg*1000000) || 
 // 							 (prefetch_policy==3 && current->argo_cached!=0)

 Policies

 0- Try to estimate when the next miss will happen and that we didnt cache the task buffer previously. Estimated miss is the 'soonest' in time estimated by starpu done by calculating when a buffer that is not cached will be scheduled. As we assume buffers are cached or not by just checking part of the data this can be is highly speculative or costly (check argo_in_cache()). If we can finish prefetch before this happens its a go. 

 1-trying to estimate when the next miss will happen based on average cachemiss times and average times on how long it takes to load task buffers into ArgoDSM cache. If we can finish prefetch before this its a go.

 2-take into account time to get cachemisses in a signal handler

 3- just prefetch if selected buffer isnt cached
*/
int prefetch_policy=0;
bool pinned_cache=true; // To restrict pages to be evicted if they are current starpu tasks
#endif



/*Treads*/
pthread_t prefetchthread;

/*Barrier*/
/** @brief  Locks access to part that does SD in the global barrier, */
pthread_mutex_t barriermutex = PTHREAD_MUTEX_INITIALIZER;
/** @brief Thread local barrier used to first wait for all local threads in the global barrier*/
pthread_barrier_t *threadbarrier;



#ifdef STARPU_SUPPORT
/**@brief Time accumulated fetching tasks in prefetcher*/
double taskacc =0.0;
/** @briefTime average it takes to prefetch a task (buffer)*/
volatile double taskavg=0.000025;
/** @briefTime average a cachemiss takes */
volatile double cachemiss_avg=0.000075;
/** @brief Last timestamp of started cachemiss */
volatile double cachemiss_start;
/** @briefLast timestamp of ended cachemiss */
volatile double cachemiss_end;
/** @briefLongest time for cachemiss */
volatile double cachemiss_high;
#endif



/*Pagecache - necessary components of the ArgoDSM cache*/
/** @brief  Size of the cache in number of pages*/
unsigned long long cachesize;
/** @brief  Offset off the cache in the backing file*/
unsigned long long cacheoffset;
/** @brief  Keeps state, tag and dirty bit of the cache*/
control_data * cachecontrol;
/** @brief  Mapping address to index, makes it easier to query the cache to see if it contains an address, works as an internal directory (althoug it can be bypassed) */
std::map<unsigned long long, unsigned long long> cachemap;
/** @brief  size of pyxis directory*/
unsigned long long classification_size;
/** @brief  The local page cache*/
char* cachedata;
#ifdef ARGO_DIFF
/** @brief Copy of the local cache to keep twinpages for later being able to DIFF stores */
char* pagecopy;
#endif
/** @brief cache size in megabytes */
unsigned long long cachesize_mb; //cachesize in mb
/** @brief Protects the pagecache */
pthread_mutex_t *cachemutex;// = PTHREAD_MUTEX_INITIALIZER;
/** @brief Protects cachemap mapping between address and index*/
pthread_mutex_t mapmutex = PTHREAD_MUTEX_INITIALIZER;
/** @brief locks per cache entry */
const unsigned long long cachelocks = 4096ul;//Number of locks per cache entry, sometimes 1:1 is too much here, used to protect access to cache entries

/*MPI Windows*/
/** @brief MPI windows for reading and writing data in global address space */
MPI_Win *global_address_window;
/** @brief MPI windows for reading and writing data in global address space in parallell to other pending data transfers*/
MPI_Win *parallel_global_address_window;
/** @brief MPI data structure for sending cache control data*/
MPI_Datatype mpi_control_data;
/** @brief number of MPI processes / ArgoDSM nodes */
unsigned long long numtasks;
/** @brief  rank/process ID in the MPI/ArgoDSM runtime*/
int workrank;
/** @brief tracking which windows are used for reading and writing global address space*/
char* data_windows_used;
/** @brief tracking which windows are used for reading and writing global address space that can be used for parallel access to the address space when you want to do prefetching or manual accesses*/
char* prefetch_windows_used;
/** @brief Semaphore protecting infiniband accesses*/
/** @todo replace with a (qd?)lock */
pthread_mutex_t window_mutex = PTHREAD_MUTEX_INITIALIZER; //sem_t ibsem; Used to be a semaphore, shouldnt matter but is needed to protect access to different windows in the argosystem. Used to also protect against accessing infiniband, but that is in later MPI versions OK to do. (Previously it would fall back silently to TCP) Only one LOCK/UNLOCK/FLUSH call etc can be done concurrently to these structures. QD locking can be perfect for this to have a helper thread do all the locking of windows and accesses, the problem is you can not safely wait/synchronize in signal handlers under POSIX. 

/*Loading and Prefetching*/
/** @brief  prefetchthread waits on this to handle prefetch requests*/
sem_t prefetchsem;
/** @brief For prefetching we want to communicate PC (instruction pointer) we miss on to the dynamic prefetcher. (irrelevant for starpu)*/
extern unsigned long long miss_addr;
/** @brief For prefetching we want to communicate address and we miss on to the dynamic prefetcher. (irrelevant for starpu)*/
extern unsigned long long miss_ip;
/** @brief 1 if prefetching is on */
char use_prefetching=0; //If prefetching is on

/*Common*/
/** @brief  Points to start of global address space*/
void * global_base_addr;
/** @brief  Points to start of global address space this process is serving */
char* globalData;
/** @brief  Size of global address space*/
unsigned long long size_of_all;
/** @brief  Size of this process part of global address space*/
unsigned long long size_of_chunk;
/** @brief  size of a page */
static const unsigned long long pagesize = 4096l;
/** @brief  In bigmem node this is the node doing computation */
int compute_node;  
/** @brief  Useful for communicating shutdown to some prefetchthreads etc, 0 if we are shutting down system */
char running = 1;
/** @brief  size of a cacheline of pages (unit pages) */
unsigned long long pageline;
/** @brief Can be used as a somewhat null reference, points outside of the address space */
unsigned long long GLOBAL_NULL;
/** @brief can be used to see if data is updated or to do dummy operations */
unsigned long long dummy = 1337;
/** @brief  linear mapping of data in the address space */
extern template class argo::data_distribution::naive_data_distribution<0>;

/** @brief  Statistics */
argo_statistics stats;
/** @brief accumulated time for load miss*/
double signalloadacc =0.0;
/** @brief avg time for load miss*/
volatile double signalloadavg =0.000475;
/** @brief accumulated time for load transfer*/
double loadlineacc =0.0;
/** @brief avg time for load transfer*/
volatile double loadlineavg =0.000045;
/** @brief Just counting how many lines prefetched*/
long prefloops = 0; 

#ifdef VA_TRACE
/** @brief  Used to count which addreses / cachelines have been used and how many times, can be useful to find statistical distribution */
unsigned int *VA_usectr;
#endif

/*debug*/
/** @brief  Placeholders to get timing information for exiting signal handlers in different places, useful to see if you have contention issues, should be named better*/
double handler1,handler2,handler3,handler4; 
/** @brief  Placeholders to get number of times exiting signal handlers in different places, useful to see if you have contention issues*/
int handler1_ret,handler2_ret,handler3_ret,handler4_ret; 

/*eviction and replacement*/
/** @brief replacement and eviction policty 0=random, 1=fifo(in terms of when a page is mapped), 2=directmapped (Modulo operation) 
 *random uses a pseudorandom number
 *fifo jsut iterates through each cacheentry so that we eventually kick something out, if something is reused we can not detect it
 * as when it is mapped it is the same as local memory
 *directmapped - just do amodulo/division operation to see what address has what index.
 */
int eviction_policy=0; //random repl
/** @brief Used for random replacement policies to get a seed */
std::random_device rd;     // only used once to initialise (seed) engine
/** @brief Used for random replacement policies, random engine*/
std::mt19937 eng(rd());    // random-number engine used (Mersenne-Twister in this case)
/** @brief Used for random replacement policies, range*/
std::uniform_int_distribution<> distr(1, 250000000); //Distribute randomize from 1 to a large number


namespace {
  /** @brief constant for invalid ArgoDSM node */
  constexpr unsigned long long invalid_node = static_cast<unsigned long long>(-1);
}
bool swdsm_is_loaded = false;
static void swdsm_loaded()  __attribute__((constructor));
void swdsm_loaded() {
  swdsm_is_loaded = true;
}
unsigned long long isPowerOf2(unsigned long long x){
  unsigned long long retval =  ((x & (x - 1)) == 0); //Checks if x is power of 2 (or zero)
  return retval;
}
bool is_compute_node(void){
#ifdef ARGO_BIGMEM_MODE
	return (compute_node == workrank);
#else
	return true;
#endif
}



/** @brief Instead of a formal write buffer, iterate through the cache for dirty copies, may be problems for caches with many entries */
void flushWriteBuffer(void){
#ifdef ARGO_NO_WRITEBACK
	return; //For testing
#endif
  unsigned long long i,j;
  double t1,t2;
  t1 = MPI_Wtime();  
	for(i = 0; i < cachesize; i++){  
		long distr_addr = cachecontrol[i].tag;
		if(distr_addr != GLOBAL_NULL){
			void * lineptr = (char*)global_base_addr + distr_addr;
			char dirty = cachecontrol[i].dirty;
			if(dirty == DIRTY){
				/*change protections to read only to make sure no one can write this while you write it back,
					can be removed if you are sure you have exclusive access*/
				mprotect(lineptr, pagesize*pageline, PROT_READ); //Maybe you want to change this to PROT_READ|PROT_WRITE depending on access rigths
				cachecontrol[i].dirty=CLEAN;
				//Loop was here if you want to iterate through a set of pages or regions and write them back separately
				//	      for(j=0; j < pageline; j++){
				//				store_line_and_sync(i,distr_addr,get_homenode(distr_addr),get_offset(distr_addr));
				store_line(i,distr_addr,get_homenode(distr_addr),get_offset(distr_addr)); //No need to sync now
				// }
			}
		}
	}
	store_line_sync();//Synchronize to MPI
  t2 = MPI_Wtime();
  stats.flushtime += t2-t1; // accum time to flush data
}

/** @brief count how many cold misses we have */
std::atomic<long> cold_miss_ctr(1);
/** @brief 
 *Remove cacheindex from the cachemap directory structure
 * this can be coupled with get_cache_index but might need to lock more resources for too long 
 * (Need to lock another cachemutex to use this which may or may not be done later on anyway)
 * decoupling mean we might not need to lock more than once
 */
void remove_cache_index(unsigned long long addr, unsigned long long index){
 	double t1;
 	t1 = MPI_Wtime();

 	if(addr == 0 || addr == GLOBAL_NULL){ 
 		return;
 	}
	unsigned long long tmpaddr = cachecontrol[index].tag;
	cachemap[tmpaddr]=-1; //set up mapping in cache
	return;
}

/** @brief 
 *Gets us a cacheindex based on address, 
 * if collision we use an eviction / replacement policy to select a new one. 
 * Eviction (and writeback happens later when a load is actually done to an index with dirty data */
long get_cache_index(unsigned long long addr, unsigned long long hint){
  unsigned long long index;
	double t1,t2;
	t1 = MPI_Wtime();

#ifdef ARGO_BIGMEM_MODE
	//If we do bigmem this is always the address we have offest in the global addr allocation. Good to keep it around if we allocate a lot.
	if(is_compute_node() && addr == 0){ 
		return 0;
	}
#endif

	std::map<unsigned long long, unsigned long long>::iterator it;
	it = cachemap.find(addr);
	if(it != cachemap.end() && cachemap.find(addr)->second != -1){ //Hit
		if(hint == 1337){ //Just so we know that StarPU task exist in cachemap
			index = -2;
			//			printf("hit - addr:%ld cached in :%ld\n",addr,it->second);
		}
		else{
			index = it->second; // found it
		}
	}
	else{ //Miss
		long mapsize = cold_miss_ctr;
		mapsize = cold_miss_ctr++;
		if(mapsize < (cachesize-pageline)){ //Cold misses, insert in incremental order
			if(hint == 1337){
				index = -1;//Just so we know that StarPU task doesnt exist in cachemap, if we fail
			}
			else{
				index=mapsize;
				cachemap[addr]=index;
			}
		}
		else { // Capacity miss
			unsigned long long random_index = 0;

			if(eviction_policy == 0){ //random
				random_index = distr(eng);
				random_index = 1+((random_index) % (cachesize-pageline));
			}
			else if(eviction_policy == 1){ //fifo
				random_index = 1+((mapsize) % (cachesize-pageline));
			}
			else if(eviction_policy == 2){ //directmapped
				random_index = 1+((addr/(pageline*pagesize)) % (cachesize-pageline));
			}
			else{ 
				printf("No eviction policy with associated id:%d\n",eviction_policy);
			}
			index = random_index; //Original cache index mapped

#ifdef STARPU_SUPPORT_SKIP_INDICES
			/** @brief  Try to only grab indices that is not part of a current / future task in starpu
			 * loops through all workers taskQs and sees if tasks in there has the index we have selected. If so, try to pick another one.
			 */
			unsigned int workers = starpu_worker_get_count();
			int i,j;
			unsigned sched_ctx_id=0;
			struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);

			for(int tries = 1; tries <= cache_collision_retry; tries++){ //try a few indices in a row in case they are locked
				index = (tries+random_index) % (cachesize-pageline); // just increment 1 from previous try
				if(pthread_mutex_trylock(&cachemutex[index%cachelocks]) != 0){ //Need to lock to get tag for comparison
					index=-1;
					break;
				}
				unsigned long long tmpaddr = cachecontrol[index].tag;
				pthread_mutex_unlock(&cachemutex[index%cachelocks]);
				if(tmpaddr >= size_of_all){
					index=-1;
					break;
				}

				bool found = false;
				bool skip = false;
				//Search for the tag among tasks
				char *search_ptr = ((char*)global_base_addr + tmpaddr);
				//Loop through all workers
				for(i = 0; i < workers; i++){
					struct _starpu_fifo_taskq *current_fifo = dt->queue_array[i]; // Work Q
					struct starpu_task_list *list = &current_fifo->taskq;
					struct starpu_task *current = list->head;
					skip = false;

					while(!skip && !found && current != NULL){
						unsigned nbuffers = current->cl->nbuffers;
						for(j=0; j<nbuffers; j++){
							starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(current, j);
							enum starpu_data_access_mode mode = STARPU_CODELET_GET_MODE(current->cl, j);
							void *local_ptr;
							size_t data_size;
							local_ptr = starpu_data_get_local_ptr(handle);
							data_size = starpu_data_get_size(handle);
							unsigned long long templine =  (long)((unsigned long long)(local_ptr) - (unsigned long long)(global_base_addr));//Get task buffer address in global address

							std::map<unsigned long long, unsigned long long>::iterator it;
							it = cachemap.find(templine); 
							if(it != cachemap.end() && cachemap.find(templine)->second != -1){ //Continue as long as we just check cached data
								if(local_ptr <= search_ptr && search_ptr <= (((char*)local_ptr)+data_size)){ //Line within task buffer
									found = true; //Available in cachemap already, try another one
									break;
								}
							}
							else{
								skip = true;
								break;
							}
						}
						current = current->next; //Try next task
					}
					if(found){
						break;
					}
				}
				if(found){
					index=-1;
				}
				else{
					cachemap[addr]=index;
				}
				return index;
			}
#endif

		}
		cachemap[addr]=index;
	}
	return index;
}

/** @brief align address to cacheline (pageline) alignment */
unsigned long long align_addr(unsigned long long addr){
  unsigned long long mod = addr % (pagesize*pageline);
	//	printf("addr:%ld mod:%ld\n",addr,mod);
  if(addr % pagesize*pageline != 0){
    addr = addr - mod;
  }
  addr /= pagesize*pageline;
  addr *= pagesize*pageline;
  return addr;
}

//#include <stdatomic.h>
void handler(int sig, siginfo_t *si, void *ptr){
  UNUSED_PARAM(sig);
  double t0 = MPI_Wtime();
	cachemiss_start=t0;
	double temp_cachemiss = (cachemiss_start-cachemiss_end);
	//Statistics about cachemisses
	if(temp_cachemiss >0 && temp_cachemiss < 2){ //Use numbers within normal boundaries
		if(temp_cachemiss > cachemiss_high && temp_cachemiss*5 > cachemiss_high){
			cachemiss_high = temp_cachemiss;
		}
		if(temp_cachemiss > cachemiss_avg){
			cachemiss_avg = 0.95*cachemiss_avg + 0.05*(temp_cachemiss);
			
		}
		else{
			cachemiss_avg = temp_cachemiss;
		}
#ifdef DEBUG
		printf("cachemiss :%lf cachemiss_start:%lf, cachemiss_end:%lf\n",temp_cachemiss,cachemiss_start,cachemiss_end);
#endif
	}
	else{
#ifdef DEBUG
		printf("negative cachemiss? timing is off!= :%lf cachemiss_start:%lf, cachemiss_end:%lf\n",temp_cachemiss,cachemiss_start,cachemiss_end);
#endif
	}

  long tag;
  char dirty;
  long distr_addr =  (long)((unsigned long long)(si->si_addr) - (unsigned long long)(global_base_addr));
	if( ((si->si_addr) < global_base_addr) || (distr_addr >=(((unsigned long long)global_base_addr)+size_of_all)))
		{		printf("EXIT workrank:%d distaddr:%ld start+sizeofall:%ld siaddr:%p startaddr:%p\n",workrank,distr_addr,((unsigned long long)global_base_addr)+size_of_all, si->si_addr, global_base_addr);		
			exit(EXIT_FAILURE);
		}

	unsigned long long line_addr = align_addr(distr_addr);
  unsigned long long * global_line_addr = (unsigned long long *)((char*)global_base_addr + line_addr);

  unsigned long long homenode = get_homenode(line_addr);
  unsigned long long offset = get_offset(line_addr);
  unsigned long long id = 1 << argo_get_nid();
  unsigned long long invid = ~id;

  double t_prefetch = MPI_Wtime();
  double t1 = MPI_Wtime();
  stats.signallock += t1-t0;
  stats.handler_requests++;

  /* page is local */
  if(homenode == (argo_get_nid())){
		//printf("LOCAL :workran%d\n",workrank);
		/* If we want sharing here we need to have one case for reading, one case for writing and update sharing accordingly. */
    int n;
		/* set page to permit read/write and map it to the page cache */
		double map1 = MPI_Wtime();
		vm::map_memory(global_line_addr, pagesize*pageline, cacheoffset+offset, PROT_READ|PROT_WRITE);
		double map2 = MPI_Wtime();
		stats.maptime += (map2-map1);
		return;
  }

  /* page is remote */
	if(pthread_mutex_trylock(&mapmutex) != 0){ //Try to take the cachemap lock
		//		printf("fail mapmutex lock workrank:%d distaddr:%ld start+sizeofall:%ld siaddr:%p startaddr:%p\n",workrank,distr_addr,((unsigned long long)global_base_addr)+size_of_all, si->si_addr, global_base_addr);		
    double t2 = MPI_Wtime();
    handler2 += t2-t1;
		handler2_ret++;
		return;//Return and get a new segfault to try again
	}

	long start_index = get_cache_index(line_addr,33);
	if(start_index == -1){
		pthread_mutex_unlock(&mapmutex);
    double t2 = MPI_Wtime();
    handler3 += t2-t1;
		handler3_ret++;
		return;
	}

	assert(start_index>=0); //index need to be positive

  unsigned long long index = start_index;
	bool tooklock = false;
	if(pthread_mutex_trylock(&cachemutex[index%cachelocks]) != 0){ //Try to take cache entry lock
		handler4_ret++;
		double t222 = MPI_Wtime();
		handler4 += t222-t1;
		pthread_mutex_unlock(&mapmutex);
		return; //Return and get a new segfault to try again
	}

#ifdef DEBUG
	printf("Miss index:%ld workrank:%d distaddr:%ld end:%ld siaddr:%p startaddr:%p\n",index,workrank,distr_addr,((unsigned long long)global_base_addr)+size_of_all, si->si_addr, global_base_addr);		
#endif

  tag = cachecontrol[index].tag;
  if(tag != line_addr){
		if(tag != GLOBAL_NULL){
			remove_cache_index(tag,index);
		}
		pthread_mutex_unlock(&mapmutex); // Other can now use cachemap directory
		pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem); Locks access to normal address space window

		miss_addr=line_addr; //Missaddress to communicate to prefetcher
		ucontext_t *uc = (ucontext_t *)ptr;
#ifdef PREFETCH_DEBUG
		printf("si:%p\n",si->si_addr);
		printf("ip:%x\n",uc->uc_mcontext.gregs[REG_RIP]);
#endif
		miss_ip=  uc->uc_mcontext.gregs[REG_RIP];//PC of miss/segfault to communicate to prefetcher
		sem_post(&prefetchsem);

    double lls0 = MPI_Wtime();
		load_line_and_sync(start_index,line_addr,get_homenode(line_addr),get_offset(line_addr)); // Load line
    double lls1 = MPI_Wtime();
		double loadlinetmp = (lls1-lls0);
		loadlineacc+=loadlinetmp;
		loadlineavg=(loadlineavg+loadlinetmp)/2.0;
		pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);


    double map1 = MPI_Wtime();
#ifdef ALWAYS_WRITABLE
    vm::map_memory(global_line_addr, pagesize*pageline, pagesize*pageline*start_index, PROT_READ|PROT_WRITE); //Map cacheline to virtual memory
#else
    vm::map_memory(global_line_addr, pagesize*pageline, pagesize*pageline*start_index, PROT_READ); //Map cacheline to virtual memory
#endif
    double map2 = MPI_Wtime();
    stats.maptime += (map2-map1);

		pthread_mutex_unlock(&cachemutex[index%cachelocks]);
    double t2 = MPI_Wtime();
		double loadtimetmp = t2-t0;
    stats.loadtime += loadtimetmp;
		
		signalloadacc += loadtimetmp;
		if(loadtimetmp > signalloadavg){
			signalloadavg = loadtimetmp;
		}
		else{
			signalloadavg = ((signalloadavg*0.9)+(loadtimetmp*0.1));
		}
#ifdef HANDLER_DEBUG
		if((stats.loads % 10000) == 0){
			printf("loads :%ld \n",stats.loads);
		}
#endif
		cachemiss_end=t2;
    return; // Load miss done
  }

#ifdef ALWAYS_WRITABLE
	//If we always map with write permission this should never happen
		printf("Write miss occurred! No write miss should happen - ALWAYS_WRITABLE defined - dual misses on same address\n");
		pthread_mutex_unlock(&cachemutex[index%cachelocks]);
		pthread_mutex_unlock(&mapmutex);
		return;
#endif

#ifdef HANDLER_DEBUG
			printf("write miss :%ld \n",line_addr);
#endif

	//Second miss on same address, must be Dirty! (We can also check this by trying to check operations, but this is not trivial).
	/*It is in some cases better to just always map to a RW state from the beginning, especially if we know tasks are written - this part will then never be used */
	cachecontrol[start_index].dirty = DIRTY; // Set line to dirty

#ifdef ARGO_DIFF
	unsigned char * real = (unsigned char *)(global_line_addr);
	unsigned char * copy = (unsigned char *)(pagecopy + index*pagesize*pageline);
	//	mprotect(global_line_addr,pagesize*pageline,PROT_READ);
	memcpy(copy,real,pageline*pagesize);
#endif 

	int tmp = mprotect(global_line_addr,pagesize*pageline,PROT_READ|PROT_WRITE);//Change rights to also get write permissions
	if(tmp == -1){
		printf("Write miss error :%d errno:%d\n",tmp,errno);
	}
	pthread_mutex_unlock(&cachemutex[index%cachelocks]);
	pthread_mutex_unlock(&mapmutex);
#ifdef HANDLER_DEBUG
	printf("write miss end :%ld \n",line_addr);
#endif

  double t2 = MPI_Wtime();
  stats.storetime += t2-t1;
  return;
}

unsigned long long get_homenode(unsigned long long addr){
  using namespace argo::data_distribution;
  global_ptr<char> gptr(reinterpret_cast<char*>(addr + reinterpret_cast<unsigned long long>(global_base_addr)));
  return gptr.node();
}

unsigned long long get_offset(unsigned long long addr){
  using namespace argo::data_distribution;
  global_ptr<char> gptr(reinterpret_cast<char*>(addr + reinterpret_cast<unsigned long long>(global_base_addr)));
  return gptr.offset();
}


void initmpi(){
	//    printf("init MPI0\n");
  int ret,initialized,thread_status;
	//int thread_level = MPI_THREAD_SINGLE;
	//int thread_level = MPI_THREAD_SERIALIZED;
	int thread_level = MPI_THREAD_MULTIPLE;
	int tmpnumtasks;
  MPI_Initialized(&initialized);
	//printf("init MPI1\n");
  if (!initialized){
    ret = MPI_Init_thread(NULL,NULL,thread_level,&thread_status);
  }
  else{
    printf("MPI was already initialized before starting ArgoDSM - shutting down\n");
    exit(EXIT_FAILURE);
  }
	//	printf("init MPI2\n");
  if (ret != MPI_SUCCESS || thread_status != thread_level) {
    printf ("MPI not able to start properly\n");
    MPI_Abort(MPI_COMM_WORLD, ret);
    exit(EXIT_FAILURE);
  }
	//	printf("init MPI3\n");

  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Comm_size(MPI_COMM_WORLD,&tmpnumtasks);
  MPI_Barrier(MPI_COMM_WORLD);
	//printf("init MPI4\n");
  MPI_Barrier(MPI_COMM_WORLD);
	numtasks = tmpnumtasks;
  MPI_Comm_rank(MPI_COMM_WORLD,&workrank);
  MPI_Barrier(MPI_COMM_WORLD);
	//printf("init MPI5\n");
  MPI_Barrier(MPI_COMM_WORLD);
	//  init_mpi_struct();
  MPI_Barrier(MPI_COMM_WORLD);
	//printf("INITED MPI numtasks:%d\n",numtasks);
  MPI_Barrier(MPI_COMM_WORLD);
	//compute_node = numtasks-1;
	compute_node = 0;
  MPI_Barrier(MPI_COMM_WORLD);
}

unsigned int argo_get_nid(){
  return workrank;
}
unsigned int argo_get_nodes(){
  return numtasks;
}

void argo_initialize(unsigned long long size){

  int i;
  unsigned long long j;

  char *prefline = getenv("ARGO_CACHELINE_SIZE");
  if(prefline == NULL){
		pageline = 256;
	}
	else{
    unsigned long long templ=atoi(prefline);
		pageline = templ;
  }
	if(pageline < 1){
		pageline = 16;
	}
	//	pageline = 512;
	printf("pageline:%d size = :%ld\n",pageline,size);
  initmpi();

  MPI_Barrier(MPI_COMM_WORLD);
  unsigned long long alignment = pagesize*pageline*numtasks;
  if((size%alignment)>0){
    size += alignment - 1;
    size /= alignment;
    size *= alignment;
  }

  global_base_addr = vm::start_address();
	//  nullpage = vm::shadow_address();//
 	printf("start:%p \n",global_base_addr);
	cachemutex = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t)*cachelocks);
	
	for(i = 0; i < cachelocks; i++){
		pthread_mutex_init(&cachemutex[i], NULL);
	}
	MPI_Barrier(MPI_COMM_WORLD);
	printf("maximum virtual memory: %ld GiB initsize %ld bytes\n", vm::size() >> 30 , size);
  MPI_Barrier(MPI_COMM_WORLD);
  threadbarrier = (pthread_barrier_t *) malloc(sizeof(pthread_barrier_t)*(NUM_THREADS+1));
  for(i = 1; i <= NUM_THREADS; i++){
    pthread_barrier_init(&threadbarrier[i],NULL,i);
  }
  unsigned long long argo_gb = 1L<<30L;
  unsigned long long argo_mb = 1L<<20L;


  char *cachestring = getenv("ARGO_CACHESIZE");
  if(cachestring == NULL){
    cachesize = 5000*argo_mb;
		cachesize /= pagesize;
		cachesize /= (pageline);
  }
  else{
		//    unsigned long long tmpgb = atoi(cachestring)/1000;
    unsigned long long tmpmb = atoi(cachestring);
		cachesize_mb = tmpmb;
    cachesize = tmpmb*argo_mb;
		cachesize /= pagesize;
		cachesize /= (pageline);
  }
	printf("cachesize %ld size:%ld\n",cachesize,cachesize*pageline*pagesize);

  char *use_pref = getenv("ARGO_USE_PREFETCHING");
  if(use_pref == NULL){
    use_prefetching=0;
  }
  else{
    use_prefetching=atoi(use_pref);
  }
  printf("user_prefetching:%ld\n", use_prefetching);

  char *env_sched_policy = getenv("ARGO_STARPU_SCHED_POLICY");
  if(env_sched_policy == NULL){
    sched_policy=0;
  }
  else{
		sched_policy=atoi(env_sched_policy);
  }
  printf("sched_policy:%ld\n", sched_policy);


  char *env_prefetch_policy = getenv("ARGO_STARPU_PREFETCH_POLICY");
  if(env_prefetch_policy == NULL){
    prefetch_policy=0;
  }
  else{
		prefetch_policy=atoi(env_prefetch_policy);
  }
  printf("prefetch_policy:%ld\n", prefetch_policy);



  char *eviction_policy_string = getenv("ARGO_EVICTION_POLICY");
  if(eviction_policy_string != NULL){
    eviction_policy = atoi(eviction_policy_string);
		printf("Evictions policy:%d\n",eviction_policy);
  }
#ifdef STARPU_SUPPORT_SKIP_INDICES
  char *cache_collision_retry_string = getenv("ARGO_CACHE_COLLISION_RETRY");
  if(cache_collision_retry_string != NULL){
    cache_collision_retry = atoi(cache_collision_retry_string);
		printf("Cache collisions retries:%d\n",cache_collision_retry);
  }
#endif

	//Used if we want to pin task-pages to never be evicted.
  char *pincache = getenv("ARGO_PINNED_CACHE");
  if(pincache != NULL){
    int eageron = atoi(pincache);
		if(eageron != 0){
			pinned_cache=true;
		}
		else{
			pinned_cache=false;
		}
  }

	classification_size = 2*cachesize; // Could be smaller ?

  data_windows_used = (char *)malloc(numtasks*sizeof(char));
  prefetch_windows_used = (char *)malloc(numtasks*sizeof(char));
  for(i = 0; i < numtasks; i++){
    data_windows_used[i] = 0;
    prefetch_windows_used[i] = 0;
  }

  if(size < pagesize*pageline*numtasks){
    size = pagesize*pageline*numtasks;
  }
  alignment = pagesize*pageline;

  if(size % (alignment*numtasks) != 0){
    size = alignment*numtasks * (1+(size)/(alignment*numtasks));
  }

  //Allocate local memory for each node,
  size_of_all = size; //total distr. global memory	
#ifdef VA_TRACE
  VA_usectr = (unsigned int *)calloc(1+(size_of_all/(pagesize*pageline)),sizeof(unsigned int));
#endif

  GLOBAL_NULL=size_of_all+1; // So we have a point of reference for something outside of the address space
  size_of_chunk = size/(numtasks); //part on each node

  sig::signal_handler<SIGSEGV>::install_argo_handler(&handler);

  unsigned long long cachecontrol_size = sizeof(control_data)*cachesize;
  cacheoffset = pagesize*pageline*(cachesize);

	//cachedata = static_cast<char*>(memalign(pagesize, cachesize*pageline*pagesize));
	cachedata = static_cast<char*>(vm::allocate_mappable(pagesize, cachesize*pageline*pagesize));
	cachecontrol = static_cast<control_data*>(memalign(pagesize, cachecontrol_size));
	//	cachecontrol = static_cast<control_data*>(vm::allocate_mappable(pagesize, cachecontrol_size));

#ifdef ARGO_DIFF
	pagecopy = static_cast<char*>(memalign(pagesize, cachesize*pagesize*pageline));
#endif

	globalData = static_cast<char*>(vm::allocate_mappable(pagesize, size_of_chunk));

  char processor_name[MPI_MAX_PROCESSOR_NAME];
  int name_len;
	
  MPI_Get_processor_name(processor_name, &name_len);
  MPI_Barrier(MPI_COMM_WORLD);

  void* tmpcache;
  tmpcache=cachedata;
#ifdef DEBUG
	printf("map cache\n");
#endif


	/*We map our addresses to the offset in the address space allocated 
		so the regions can be known relatively to one point in remote nodes */

	//	if(is_compute_node()){ //In big memory mode, maybe only compute node need to map the memory
	vm::map_memory(tmpcache, pagesize*pageline*cachesize, 0, PROT_READ|PROT_WRITE); //Mapping our cache here
	//		}
  unsigned long long current_offset = pagesize*pageline*cachesize;

	/*If we want to map controldata so it is addressable from remote nodes*/
  //tmpcache=cachecontrol;
	//printf("map cache control\n");
	//		if(is_compute_node()){
	//vm::map_memory(tmpcache, cachecontrol_size, current_offset, PROT_READ|PROT_WRITE);
	//	}
	//  current_offset += cachecontrol_size;


  tmpcache=globalData;
	//	if(!is_compute_node()){//In big memory mode, maybe only compute node need to map the memory
	vm::map_memory(tmpcache, size_of_chunk, current_offset, PROT_READ|PROT_WRITE); //Mapping our part of the global address space to a local buffer
	//		}

  MPI_Barrier(MPI_COMM_WORLD); //Good to have reference points where all nodes have passed during init, easier for debugging

	/* Allocate address windows to access global data */
  global_address_window = (MPI_Win*)malloc(sizeof(MPI_Win)*numtasks);
  for(i = 0; i < numtasks; i++){
#ifdef DEBUG
		printf("globaldatwin worker:%d size:%ld myrank:%d\n",i,size_of_chunk,workrank);
#endif
    MPI_Win_create(globalData, size_of_chunk*sizeof(char), 1,
									 MPI_INFO_NULL, MPI_COMM_WORLD, &global_address_window[i]);
		MPI_Barrier(MPI_COMM_WORLD);
  }
  MPI_Barrier(MPI_COMM_WORLD);
	/* Allocate address windows used for prefetching and concurrent accesses to the address space*/
	parallel_global_address_window = (MPI_Win*)malloc(sizeof(MPI_Win)*numtasks);
  MPI_Barrier(MPI_COMM_WORLD);
	for(i = 0; i < numtasks; i++){
		MPI_Win_create(globalData, size_of_chunk*sizeof(char), 1,
	 								 MPI_INFO_NULL, MPI_COMM_WORLD, &parallel_global_address_window[i]);
		MPI_Barrier(MPI_COMM_WORLD);
	}
  MPI_Barrier(MPI_COMM_WORLD);

	/* Make sure cache control data is clean */
	if(is_compute_node()){
		/*init prefetchers*/
		if(prefetch_policy <4){ //3 different policies for starpu
#ifdef STARPU_SUPPORT
			pthread_create(&prefetchthread,NULL,&starpu_prefetcher,(void*)NULL); //StarPU prefetcher
#endif
		}
		else if(prefetch_policy>4){
			pthread_create(&prefetchthread,NULL,&prefetcher,(void*)NULL);//Dynamic prefetcher
		}
		else{
			//policy 4 = no pref
		}
		init_prefetcher(); //init dynamic prefetcher

	}
  MPI_Barrier(MPI_COMM_WORLD);
  argo_reset_coherence(1); //Reset coherence, 1 local thread
  MPI_Barrier(MPI_COMM_WORLD);

	mprotect(cachedata, pagesize*pageline*cachesize, PROT_READ|PROT_WRITE); // Cache should be in RW permission

}

void argo_finalize(){
  int i;
  printf("ArgoDSM shutting down\n");
  MPI_Barrier(MPI_COMM_WORLD);
  printf("ArgoDSM shutting down passed barrier\n");
	print_statistics();

  running = 0;
	//	pthread_join(prefetchthread, NULL);
  MPI_Barrier(MPI_COMM_WORLD);
  printf("ArgoDSM shut down - COMPLETE\n");
	exit(EXIT_SUCCESS);

  return;
}

void self_invalidation(){
#ifdef debug
	printf("si\n");
#endif

  unsigned long long i;
  double t1,t2;
  int flushed = 0;
  unsigned long long id = 1 << argo_get_nid();

  t1 = MPI_Wtime();
  for(i = 0; i < cachesize; i++){
    unsigned long long distr_addr = cachecontrol[i].tag;
    unsigned long long line_addr = distr_addr;
		if(line_addr > size_of_all){continue;}
    long classidx = get_classification_index(line_addr);
    char dirty = cachecontrol[i].dirty;
		/* If something is dirty we need to write it back before self invalidation, however, 
			 these might not be in sync because barrier is done before SI. 
			 May have to put a barrier after SI if this is a problem */
    if(flushed == 0 && dirty == DIRTY){
      flushWriteBuffer();
      flushed = 1;
    }
		cachecontrol[i].dirty=CLEAN;
    cachecontrol[i].tag = GLOBAL_NULL;
    mprotect((char*)global_base_addr + line_addr, pagesize*pageline, PROT_NONE);
  }

  t2 = MPI_Wtime();
  stats.selfinvtime += (t2-t1);
}
		
/** @brief global argo barrier synchronizes data
 *@param n Local threads within an argo node entering the barrier
*/
void swdsm_argo_barrier(int n){ //BARRIER
	double time1,time2;
	pthread_t barrierlockholder;
	time1 = MPI_Wtime();
	pthread_barrier_wait(&threadbarrier[n]); //First do a local barrier

	if(argo_get_nodes()==1){ // If we are only using 1 argo node we do not have to sync
		time2 = MPI_Wtime();
		stats.barriers++;
		stats.barriertime += (time2-time1);
		return;
	}

	if(pthread_mutex_trylock(&barriermutex) == 0){
		barrierlockholder = pthread_self();
		pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);

#ifdef ARGO_BIGMEM
		//Dont want to writeback since there isnt any coherence(??)
#else
		flushWriteBuffer(); //SD fence
		MPI_Barrier(MPI_COMM_WORLD); // Synchronization point, barrier over MPI
#endif
		if(is_compute_node()){
			self_invalidation(); //SI-fence
		}
		pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);
	}

	pthread_barrier_wait(&threadbarrier[n]); //Local barrier, local threads wait here
	if(pthread_equal(barrierlockholder,pthread_self())){
		pthread_mutex_unlock(&barriermutex);
		time2 = MPI_Wtime();
		stats.barriers++;
		stats.barriertime += (time2-time1);
	}
}
/** @brief resets coherence information, synchronizes alla argo nodes - called collectively
 * @param n local threads of calling argo node
*/
void argo_reset_coherence(int n){
#ifdef DEBUG
	printf("reset coherence\n");
#endif
  int i;
  unsigned long long j;

	for(j=0; j<cachesize; j++){ //Clear cache meta information tags, dirty etc
		cachecontrol[j].tag = GLOBAL_NULL;
#ifdef CACHE_TRACE
		cachecontrol[j].usectr = 0;
#endif
		cachecontrol[j].dirty = CLEAN;
	}
	
	swdsm_argo_barrier(n); // SYNC with barrier
  clear_statistics(); //Clear stats
	mprotect(cachedata, pagesize*pageline*cachesize, PROT_READ|PROT_WRITE); //Cache should always be RW

#ifdef ARGO_BIGMEM_MODE 
	if(is_compute_node()){
		mprotect(global_base_addr,size_of_all,PROT_NONE); 
		mprotect(global_base_addr,pagesize,PROT_READ|PROT_WRITE); //This is offset to allocation in global memory so we dont always need to bring this in
	}
	else{
		mprotect(global_base_addr,size_of_all,PROT_READ|PROT_WRITE);
	}
#else
		mprotect(global_base_addr,size_of_all,PROT_NONE); 
#endif
}

	/* SI fence */
void argo_acquire(){
  int flag;
#ifdef DEBUG
	printf("argoacq\n");
#endif
	//  pthread_mutex_lock(&cachemutex[index%cachelocks]);
  pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);
  self_invalidation();
  MPI_Iprobe(MPI_ANY_SOURCE,MPI_ANY_TAG,MPI_COMM_WORLD,&flag,MPI_STATUS_IGNORE); //May be necessary to enter MPI library
  pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);
	//  pthread_mutex_unlock(&cachemutex[index%cachelocks]);
}

	/* SD fence */
void argo_release(){
  int flag;
#ifdef DEBUG
	printf("argorel\n");
#endif
	//  pthread_mutex_lock(&cachemutex[index%cachelocks]);
  pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);
  flushWriteBuffer();
MPI_Iprobe(MPI_ANY_SOURCE,MPI_ANY_TAG,MPI_COMM_WORLD,&flag,MPI_STATUS_IGNORE); //May be necessary to enter MPI library
  pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);
	//pthread_mutex_unlock(&cachemutex[index%cachelocks]);
}

	/* SI + SD fence */
void argo_acq_rel(){
  argo_acquire();
  argo_release();
}


double argo_wtime(){
  return MPI_Wtime();
}

/* Clear statistics - set everything to 0*/
void clear_statistics(){
  stats.selfinvtime = 0;
  stats.loadtime = 0;
  stats.signallock = 0;
  stats.evicttime = 0;
  stats.storetime = 0;
  stats.flushtime = 0;
  stats.writebacktime = 0;
  stats.locktime=0;
  stats.barriertime = 0;
  stats.stores = 0;
  stats.writebacks = 0;
  stats.loads = 0;
  stats.handler_requests = 0;
  stats.barriers = 0;
  stats.locks = 0;
  stats.loadtransfertime =0;
  stats.loadtransferavg =0;
  stats.prefetchesissued =0;
  stats.maptime =0;
	stats.evictions=0;
	stats.searchcachetime=0.0;
	handler1=0.0;
	handler2=0.0;
	handler3=0.0;
	handler4=0.0;
	handler1_ret=0;
	handler2_ret=0;
	handler3_ret=0;
	handler4_ret=0;
	taskacc =0.0;
	signalloadacc =0.0;
	loadlineacc =0.0;
}


void print_statistics(){
  printf("\n\n\n- - - - STATISTICS - - - -\n");
	printf("dummy:%ld\n",dummy);
  printf("node:%d\n",workrank);
  printf("cachesize:%ld\n",cachesize);

  printf("handler requests:%lu\n",stats.handler_requests);
  printf("handler1time:%lf ret:%d\n",handler1,handler1_ret);
  printf("handler2time:%lf ret:%d\n",handler2,handler2_ret);
  printf("handler3time:%lf ret:%d\n",handler3,handler3_ret);
  printf("handler4time:%lf ret:%d\n",handler4,handler4_ret);

  printf("loads:%lu\n",stats.loads);
  printf("loadtime:%lf\n",stats.loadtime);
  printf("maptime:%lf\n",stats.maptime);
  printf("loadtransfertime:%lf\n",stats.loadtransfertime);
  printf("loadtransferavg:%lf\n",stats.loadtransferavg);

  printf("evictions:%ld\n",stats.evictions);
  printf("evicttime:%lf\n",stats.evicttime);

  printf("prefetchesissued:%d\n",stats.prefetchesissued);

  printf("stores:%lu\n",stats.stores);
  printf("writebacks:%lu\n",stats.writebacks);
  printf("storetime:%lf\n",stats.storetime);
  printf("writebacktime:%lf\n",stats.writebacktime);
  printf("signallocktime:%lf\n",stats.signallock);

  printf("argo searchcachetime:%lf\n",stats.searchcachetime);
  printf("argo task acc:%lf\n",taskacc);
  printf("argo task avg:%lf\n",taskavg);

  printf("argo signal acc:%lf\n",signalloadacc);
  printf("argo signal avg:%lf\n",signalloadavg);

  printf("argo loadline acc:%lf\n",loadlineacc);
  printf("argo loadline avg:%lf\n",loadlineavg);

	printf("prefloops:%ld\n",prefloops);

  printf("\n\n\n");

#ifdef VA_TRACE
  printf("cacheuse\n");
  for(unsigned long long j=0; j<(1+(size_of_all/(pagesize*pageline))); j++){
    printf("Node:%d, distraddr:%ld uses:%ld\n",workrank,j*pagesize*pageline, VA_usectr[j]);
  }
  printf("\n\n\n");
#endif

#ifdef CACHE_TRACE
  printf("cacheuse-\n");
  for(unsigned long long j=0; j<cachesize; j++){
    printf("Node:%d, idx:%ld uses:%ld\n",workrank,j,		cachecontrol[j].usectr);
  }
  printf("\n\n\n");
#endif

}
/* global base address */
void *argo_get_global_base(){return global_base_addr;}
/* global address space size */
unsigned long long argo_get_global_size(){return size_of_all;}

/*returns an index used for the passive classification directory, direct mapped */
unsigned long long get_classification_index(unsigned long long addr){
  return (2*(addr/(pagesize*pageline))) % classification_size;
}


/** @brief  Check if this region is in our cache 1 if cached / local, 0 otherwise. 
 *We assume entire region cached if we find one(first) line cached 
 *can easily extend to first and last, or pick a few at random, etc.
 *We check if line is cached by looking up in our cachemap directory and see if the tag is correct, if busy, assume not cached.
 *@param ptr, pointer to region
 *@param size size of region in bytes
*/

int argo_in_cache(void* ptr, unsigned long size){
	double t1 = MPI_Wtime();	
  unsigned long long distr_addr =  (long)((unsigned long long)(ptr) - (unsigned long long)(global_base_addr)); //Logical address
  unsigned long long line_addr = align_addr(distr_addr); //Align the address to page

	if(workrank == get_homenode(distr_addr)){ // If this is local, its same as if its cached
		return 1;
	}
	bool found = false;

	pthread_mutex_lock(&mapmutex); //Cachemap need to be protected
	std::map<unsigned long long, unsigned long long>::iterator it;
	it = cachemap.find(line_addr); // Find line
	if(it != cachemap.end() && cachemap.find(line_addr)->second != -1){
		unsigned long long index = cachemap.find(line_addr)->second;
		if(pthread_mutex_trylock(&cachemutex[index%cachelocks]) != 0){ //Cant take the cachelock, assume not cached so we dont spin here.
			pthread_mutex_unlock(&mapmutex);
			return 0;
		}
		if(cachecontrol[index].tag==line_addr){//Tag is correct
			found=true; //Cached
		}
		pthread_mutex_unlock(&cachemutex[index%cachelocks]);
	}
	pthread_mutex_unlock(&mapmutex);
	double t2 = MPI_Wtime();
	stats.searchcachetime+=(t2-t1);

	//Return cached or not
 	if(found){
 		return 1;
 	}
 	else{
 		return 0;
 	}
}

/** @brief  prefetcher connected to starpu  */
void *starpu_prefetcher(void *x){
	int core_id =7;//Works for Rackham well if we use it for just scaling memory
	int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
	printf("numcores:%d core_id:%d\n",num_cores,core_id);
	if (core_id < 0 || core_id >= num_cores){
		printf("PINNING ERROR core id wrong\n");
    argo_finalize();
		return NULL;
	}
	
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(core_id, &cpuset);
	pthread_t current_thread = pthread_self();
	int s = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset); // Pin to core 'core_id', may or may not be a good idea depending on use
	
#ifdef DEBUG
	printf("s:%d thread:%d\n",s,current_thread);
#endif
	
  if(s != 0){
		printf("PINNING ERROR\n");
		argo_finalize();
	}
	
	
  while(running && use_prefetching){
		double t0; // for timing 
		while(running){
			char* ptr;
			unsigned long long size;
			char mode;
			int workerid = 0;
			sleep(1); //Just make sure starpu started ... need to fix this - but we are running for hrs so not that significant
			printf("START PREFETCH\n");

			unsigned int workers = starpu_worker_get_count();
			unsigned int loopcount=0;

			while(running){ //Loop until we are shutting down
				int i,j;
				unsigned sched_ctx_id=0;
				loopcount++;
				usleep(100); //dont busy spin
				t0 = MPI_Wtime();
				struct _starpu_dmda_data *dt = (struct _starpu_dmda_data*)starpu_sched_ctx_get_policy_data(sched_ctx_id);
				float soonest_miss;
				for(i = 0; i < workers; i++){
					//Worker queue and its expected cache miss 
					float cachemiss = dt->queue_array[workerid]->exp_cachemiss;
					if(i==0 || cachemiss < soonest_miss){
						soonest_miss = cachemiss;
					}
				}
				if(workers > 0){
					workerid = (workerid+1) % workers; //Go around all workers
					int taskcount =0;
					
					struct _starpu_fifo_taskq *current_fifo = dt->queue_array[workerid]; //Task queue
					struct starpu_task_list *list = &current_fifo->taskq;
					struct starpu_task *current = list->head;
					int len = current_fifo->ntasks;
					float ratio =0;
					
					bool found = false;
					float time = current_fifo->exp_start;
					bool iscached=true;
					unsigned long long endline, totallines;

					//Loop through this workers taskq to see if we can prefetch anything, taskcount means depth in the taskQ here
					while(running && current != NULL 	//&& taskcount <5
								){ //If 10 tasks is larger than your cache this might need to be lower
						
						/*See top of file for description of prefetch policies*/
						if(
							 (prefetch_policy==0 && soonest_miss > starpu_timing_now()+taskavg*1000000 && current->argo_cached!=0) ||
							 (prefetch_policy==1 && cachemiss_start+argo_get_signalavg()+cachemiss_avg > MPI_Wtime()+taskavg && current->argo_cached!=0) ||
							 (prefetch_policy==2 && time > starpu_timing_now()+taskavg*1000000 && current->argo_cached!=0 && cachemiss_start+argo_get_signalavg()+cachemiss_avg > MPI_Wtime()+taskavg && soonest_miss > starpu_timing_now()+taskavg*1000000) || 
							 (prefetch_policy==3 && current->argo_cached!=0)					 
							 ){
#ifdef DEBUG							
							printf("\n\ntaskcount:%d argocached:%d time:%lf, soonest_miss:%lf, starpu+task:%lf taskavg:%lf nowmpi+task:%lf cachemissend:%lf cachemissavg:%lf cachemisshigh:%lf, cachemissstart:%lf signalloadavg:%lf\n",taskcount, current->argo_cached,time,soonest_miss, starpu_timing_now()+taskavg, taskavg,MPI_Wtime()+taskavg, cachemiss_end, cachemiss_avg, cachemiss_high,cachemiss_start, argo_get_signalavg());
#endif
							unsigned nbuffers = current->cl->nbuffers; // Number of buffers in Starpu, we support max 3 now per Task
							for(j=0; j<nbuffers; j++){ //Maybe we should not start on buffer 0, instead we could pick a random buffer to start with
								int cached = current->argo_cached;
								/*Each starpu task is assumed to have 3 buffers,
									argo_cached value is computed by buffer0=1, buffer1=2, buffer2=4 
									if any of them is NOT cached*/
								if(j==0 && (cached==1 || cached==3 || cached==5 || cached==7)){ //if buffer0 is not cached
									//First buffer
								}  
								else if(j==1 && (cached==2 || cached==3 || cached==6 ||  cached==7)){ //Buffer1 is not cached
									//Second buffer
								}  
								else if(j==2 && (cached==4 || cached==5 || cached==6 ||  cached==7)){ //Buffer2 is not cached
									//Third buffer
								}  
								else { // All cached, continue
									//printf("else continue cached:%d\n",cached);
									continue;
								}  
								
								starpu_data_handle_t handle = STARPU_TASK_GET_HANDLE(current, j);
								char *local_ptr = (char *)starpu_data_get_local_ptr(handle);
								unsigned long long data_size = starpu_data_get_size(handle);
								if(global_base_addr<local_ptr && local_ptr <(global_base_addr+size_of_all)){ //Valid address to prefetch
									
									unsigned long long distr_addr =  (long)((unsigned long long)(local_ptr) - (unsigned long long)(global_base_addr));
									unsigned long long task_addr = align_addr(distr_addr);
									totallines = ((data_size+(pagesize*pageline)-1)/ (pagesize*pageline)); //Task-buffer is these many lines
									
									int lines = totallines;
									float time_until_miss = cachemiss_start+argo_get_signalavg()+cachemiss_avg - MPI_Wtime(); //Estimated time until cachemiss
									
									if(prefetch_policy != 0){
										//Take into accout cache miss times
										lines = (int)(time_until_miss/taskavg);
										if(time_until_miss < 0){break;}
									}
									//If we assume that we dont have time to do all the lines, just do as many as we think we can
									if(lines > totallines){
										lines = totallines;
									}

									endline=lines; //Update loop boundary

									long task_cacheindex;
									if(pthread_mutex_trylock(&mapmutex) == 0){
										task_cacheindex = get_cache_index(task_addr,1337); //1337 is 
										pthread_mutex_unlock(&mapmutex);
									}
									else{
										task_cacheindex = -3;
									}
									
									if(task_cacheindex<1){ // Different error numbers if we cant get a positive cacheindex (0 index is saved for internal use)
										continue;
									}
									
									bool mprotted = false;
									bool success=true;
									for(i=0; i<lines; i++){
										long cacheindex = task_cacheindex+i;
										//lock cachelines
										if(pthread_mutex_trylock(&cachemutex[cacheindex%cachelocks]) != 0){ 
											endline = i;
											//success = false;
											break;
										}
									}
									if(!success){//Dont bother prefetch if we have contention - fetch most likely already in place
										for(i=0; i<endline; i++){
											long cacheindex = task_cacheindex+endline-1-i;
											pthread_mutex_unlock(&cachemutex[cacheindex%cachelocks]);
										}
										break;
									}
									
									for(i=0; i<endline; i++){
										long cacheindex = task_cacheindex+i;
										unsigned long long tmptag = cachecontrol[cacheindex].tag;
										if(tmptag != GLOBAL_NULL && tmptag != task_addr+i*pagesize*pageline && cachecontrol[cacheindex].dirty==DIRTY){
											if(!mprotted){
												mprotted=true;
												//older versions of MPI will have to serialize all infiniband accesses
												//pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);
											}
											mprotect(global_base_addr+tmptag,pagesize*pageline,PROT_READ);
											store_line_prefetch(cacheindex, tmptag, get_homenode(tmptag), get_offset(tmptag));
										}
									}
									if(mprotted){
										found = true;
										end_prefetch_epoch();
										pthread_mutex_lock(&mapmutex);
										for(i=0; i<endline; i++){
											long cacheindex = task_cacheindex+i;
											unsigned long long tmptag = task_addr+i*pagesize*pageline;
											if(tmptag != GLOBAL_NULL){
												remove_cache_index(tmptag,cacheindex); //Remove mappings in cachemap 'directory'
											}
										}
										pthread_mutex_unlock(&mapmutex);
#ifdef DEBUG
										printf("prefetching :(%d,%d)\n",task_cacheindex,task_cacheindex+i);
#endif
										for(i=0; i<endline; i++){
											long cacheindex = task_cacheindex+i;
											unsigned long long tmptag = task_addr+i*pagesize*pageline;
											prefetch_line(cacheindex, tmptag, get_homenode(tmptag), get_offset(tmptag));
										}
										end_prefetch_epoch();
										//older versions of MPI will have to serialize all infiniband accesses
										//pthread_mutex_unlock(&window_mutex); 
										double map1 = MPI_Wtime();
#ifdef ALWAYS_WRITABLE
										vm::map_memory(global_base_addr+task_addr, pagesize*pageline*endline, pagesize*pageline*task_cacheindex, PROT_READ|PROT_WRITE);
#else
										vm::map_memory(global_base_addr+task_addr, pagesize*pageline*endline, pagesize*pageline*task_cacheindex, PROT_READ);
#endif
										double map2 = MPI_Wtime();
										stats.maptime += (map2-map1);
										prefloops+=endline;
										current->argo_cached=0;
										current->argo_prefetched=2;
										if(endline == lines){
											if(j==0){
												current->argo_cached-=1;
											} 
											if(j==1){
												current->argo_cached-=2;
											} 
											if(j==2){
												current->argo_cached-=4;
											} 
										}
									}
									for(i=0; i<endline; i++){ //Unlock cachelines
										long cacheindex = task_cacheindex+endline-1-i;
										pthread_mutex_unlock(&cachemutex[cacheindex%cachelocks]);
									}
								}
								break; //For now, just do 1 buffer per prefetch
							}
							break;
						}
						else if(current->argo_cached!=0){
							break;
						}
						else{
							time += current->predicted;
							time += current->predicted_transfer;
						}
						current = current->next;
						taskcount++;
					}

					double tasktime = MPI_Wtime()-t0;
					taskacc+=tasktime;
					if(found){
						float linetime = tasktime/((float)endline);
						//Below one is considered a 'sane' number, everything else are fringe values, need to take care of these long tasks separately
						if(tasktime < 1){
							tasktime = linetime*totallines;
							if(tasktime > taskavg){
								taskavg = tasktime;
							}
							else{
								//Weight towards the 'avg' so we dont get rapidly changing values
								taskavg = (taskavg*0.9)+(tasktime*0.1);
							}
#ifdef DEBUG
							printf("tasktime:%lf, taskavg:%lf workerid:%d taskcount:%d cachemiss_avg:%lf ,expstart:%lf, expcachemiss:%lf time:%lf \n",tasktime,taskavg,workerid,taskcount,cachemiss_avg,current_fifo->exp_start,current_fifo->exp_cachemiss,time);
#endif
						}
						else{
#ifdef DEBUG
							printf("UNUSUAL (long) TASKTIME tasktime:%lf, taskavg:%lf workerid:%d taskcount:%d cachemiss_avg:%lf \n",tasktime,taskavg,workerid,taskcount,cachemiss_avg);
#endif
						}
					}
#ifdef DEBUG
					if((loopcount%10000)==0){
						printf("intermediate data - tasktime:%lf, taskavg:%lf workerid:%d taskcount:%d cachemiss_avg:%lf expstart:%lf, expcachemiss:%lf\n",tasktime,taskavg,workerid,taskcount,cachemiss_avg,current_fifo->exp_start,current_fifo->exp_cachemiss);
					}
#endif
				}
				else{
					//Workers might not have started yet, poll until we get non-zero
					workers = starpu_worker_get_count();
				}
			}
			return NULL;
		}
	}
	return NULL;
}

/** @brief  Clear state of argo */
void argo_clear_state(){
	usleep(100);
	printf("---argo_clear_state---\n");
	prefloops=0;
	cachemap.clear(); //Clear 'directory'
	cold_miss_ctr=1;
	
	for(int i = 0; i < cachesize; i++){
		//maybe this need to be locked depending on use case
		//		pthread_mutex_lock(&cachemutex[i%cachelocks]); 
		cachecontrol[i].tag = GLOBAL_NULL;
		cachecontrol[i].dirty = CLEAN;
		//pthread_mutex_unlock(&cachemutex[i%cachelocks]);
	}	
	usleep(100);
	// Unmap all regions
	mprotect(global_base_addr,size_of_all,PROT_NONE);

	clear_statistics();
	printf("Clear State  - initialization evs:%ld\n", stats.evictions);
}

double argo_get_taskavg(){
	return taskavg;
}

double argo_get_signalavg(){
	return signalloadavg;
}

int argo_get_sched_policy(){
	return sched_policy;
}
