#include "prefetcher.hpp"
extern int prefetch_policy;

extern control_data * cachecontrol; //control data cache
extern pthread_mutex_t *cachemutex; //cache locks 
extern pthread_mutex_t mapmutex; //Lock for eviction/replacement policy 'dir' structure

extern void* global_base_addr;
extern unsigned long size_of_all;
extern sem_t prefetchsem;
extern int workrank;
extern unsigned long pageline;
static const unsigned long pagesize = 4096;
const unsigned long long cachelocks = 20000ul;

/*these should be synced and perhaps be queues?*/
/** @brief miss address communicated from signal handler */
unsigned long miss_addr = 0;
/** @brief miss instr pointer / PC communicated from signal handler */
unsigned long miss_ip = 0;

extern long GLOBAL_NULL;
extern argo_statistics stats;

#ifndef LOOKAHEAD
#define LOOKAHEAD 1
#endif

#define ARGO_PREFETCH_DEBUG 1
namespace vm = argo::virtual_memory;
typedef struct ip_tracker
{
		// the IP related pid(thread id)
		//pid_t pid;

		// the IP we're tracking
		unsigned long long int ip;

		// the last address accessed by this IP
		unsigned long long int last_addr;

		// the stride between the last two addresses accessed by this IP
		long long int last_stride;

		// use LRU to evict old IP trackers
		unsigned long long int lru_cycle;

} ip_tracker_t;

ip_tracker_t trackers[IP_TRACKER_COUNT];




typedef struct ampm_zone
{
		// Zone address
		unsigned long int zone;

		// The access map itself.
		// Each element is set when the corresponding cache line is accessed.
		// The whole structure is analyzed to make prefetching decisions.
		// While this is coded as an integer array, it is used conceptually as a single 64-bit vector.
		int access_map[AMPM_PAGES_IN_ZONE];

		// This map represents cache lines in this page that have already been prefetched.
		// We will only prefetch lines that haven't already been either demand accessed or prefetched.
		int pf_map[AMPM_PAGES_IN_ZONE];

		// used for page replacement
		unsigned long long int lru;

} ampm_zone_t;

ampm_zone_t ampm_zones[AMPM_ZONES];

/*stream prefetch*/
char direction = 0;
unsigned int stream_confidence = 0;

/*general*/
unsigned long lru_counter = 0;
unsigned long previous_miss = 0;



void init_prefetcher(){
  int i;
  for(i=0; i<AMPM_ZONES; i++){
    ampm_zones[i].zone = 0;
    ampm_zones[i].lru = 0;

    int j;
    for(j=0; j<AMPM_PAGES_IN_ZONE; j++){
      ampm_zones[i].access_map[j] = 0;
      ampm_zones[i].pf_map[j] = 0;
    }
  }
  for(i=0; i<IP_TRACKER_COUNT; i++){
    //    trackers[i].pid = 0;
    trackers[i].ip = 0;
    trackers[i].last_addr = 0;
    trackers[i].last_stride = 0;
    trackers[i].lru_cycle = 0;
  }
  //...
}

/** @brief stream prefetching Adapted from http://comparch-conf.gatech.edu/dpc2/ */
bool stream_plus(unsigned long addr){ //Just stream prefetch ahead without confidence check

	unsigned long long int pf_addr = (addr + (pagesize*pageline)); //Where to start to fetch from
	if(pthread_mutex_trylock(&mapmutex) != 0){return false;} 
	long pf_index = get_cache_index(pf_addr,0);
	pthread_mutex_unlock(&mapmutex);
	if(pf_index <= 0){return false;}

	unsigned long endline = LOOKAHEAD;
	unsigned long lines = LOOKAHEAD;
	bool mprotted = false;
	bool success=true;
	int i;
	for(i=0; i<lines; i++){
		long cacheindex = pf_index+i;

		//lock cachelines
		if(pthread_mutex_trylock(&cachemutex[cacheindex%cachelocks]) != 0){ 
			endline = i;
			success = false;
			break;
		}
	}

	if(!success){//Dont bother prefetch if we have contention - fetch most likely already in place
		for(i=0; i<endline; i++){
			long cacheindex = pf_index+endline-1-i;
			pthread_mutex_unlock(&cachemutex[cacheindex%cachelocks]);
		}
		return false;
	}

	for(i=0; i<endline; i++){
		stats.prefetchesissued++;
		long cacheindex = pf_index+i;
		unsigned long long tmptag = cachecontrol[cacheindex].tag;
		if(tmptag != GLOBAL_NULL && tmptag != pf_addr+i*pagesize*pageline && cachecontrol[cacheindex].dirty==DIRTY){
			if(!mprotted){
				mprotted=true;
				//												sem_wait(&ibsem);
			}
			mprotect(global_base_addr+tmptag,pagesize*pageline,PROT_READ);
			store_line_prefetch(cacheindex, tmptag, get_homenode(tmptag), get_offset(tmptag));
		}
	}

	if(mprotted){
		end_prefetch_epoch();
		pthread_mutex_lock(&mapmutex);
		for(i=0; i<endline; i++){
			long cacheindex = pf_index+i;
			unsigned long long tmptag = pf_addr+i*pagesize*pageline;
			if(tmptag != GLOBAL_NULL){
				remove_cache_index(tmptag,cacheindex); //Remove mappings in cachemap 'directory'
			}
		}
		pthread_mutex_unlock(&mapmutex);

		for(i=0; i<endline; i++){
			printf("prefetching :(%d,%d)\n",pf_index,pf_index+i);
			long cacheindex = pf_index+i;
			unsigned long long tmptag = pf_addr+i*pagesize*pageline;
			prefetch_line(cacheindex, tmptag, get_homenode(tmptag), get_offset(tmptag));
		}
		end_prefetch_epoch();
		double map1 = MPI_Wtime();
//maybe you want to prefetch this to RW protection immediately (Set PROT_READ|PROT_WRITE and cache_control[cacheindex].dirty = DIRTY) Also need to set up pagecopies if we want to be able to do partial write backs. (Diffs)
		vm::map_memory(global_base_addr+pf_addr, pagesize*pageline*endline, pagesize*pageline*pf_index, PROT_READ); 
		double map2 = MPI_Wtime();
	}

	for(i=0; i<endline; i++){ //Unlock cachelines
		long cacheindex = pf_index+endline-1-i;
		pthread_mutex_unlock(&cachemutex[cacheindex%cachelocks]);
	}
	return true;
}

/** @brief PC-based prefetching Adapted from http://comparch-conf.gatech.edu/dpc2/ */
void ip_prefetch(unsigned long addr, unsigned long ip){  
  // check for a tracker hit
  int tracker_index = -1;
  if(!ip){return;}

  lru_counter++;
  int i;
  for(i=0; i<IP_TRACKER_COUNT; i++){
    if(trackers[i].ip == ip ){
      trackers[i].lru_cycle = lru_counter;
      tracker_index = i;
      break;
    }
  }

  if(tracker_index == -1){
    // this is a new IP that doesn't have a tracker yet, so allocate one
    int lru_index=0;
    unsigned long long int lru_cycle = trackers[lru_index].lru_cycle;
    int i;
    for(i=0; i<IP_TRACKER_COUNT; i++){
			if(trackers[i].lru_cycle < lru_cycle){
				lru_index = i;
				lru_cycle = trackers[lru_index].lru_cycle;
			}
		}
    tracker_index = lru_index;

    // reset the old tracker
    trackers[tracker_index].ip = ip;
    trackers[tracker_index].last_addr = addr;
    trackers[tracker_index].last_stride = 0;
    trackers[tracker_index].lru_cycle = lru_counter;
    return;
  }

  // calculate the stride between the current address and the last address
  // this bit appears overly complicated because we're calculating
  // differences between unsigned address variables
  long long int stride = 0;
  if(addr > trackers[tracker_index].last_addr){
    stride = addr - trackers[tracker_index].last_addr;
  }
  else{
    stride = trackers[tracker_index].last_addr - addr;
    stride *= -1;
  }

  // don't do anything if wesaw the same address twice in a row
  if(stride == 0){
#ifdef ARGO_PREFETCH_DEBUG
		printf("Stride 0\n");
#endif
    return;
  }

  // only do any prefetching if there's a pattern of seeing the same
  // stride more than once for same PC
  if(stride == trackers[tracker_index].last_stride){

#ifdef ARGO_PREFETCH_DEBUG
		printf("Stride :%d - do prefetch\n",stride);
#endif

    int i;
		unsigned long long int pf_addr = (addr + stride)%size_of_all;
		unsigned long endline = LOOKAHEAD;
		unsigned long lines = LOOKAHEAD;
		bool mprotted = false;
		bool success=true;
		
		long *indices = (long*)malloc(sizeof(long)*lines); //Make sure we can temporary store indices of lines as they might appear in non-linear order

		for(i=0; i<lines; i++){
			if(pf_addr+stride*i >= size_of_all || pf_addr+stride*i <= 0){
				endline = i;
				success = false;
				break;
			}
			if(pthread_mutex_trylock(&mapmutex) != 0){
				endline = i;
				success = false;
				break;
			} 
			long pf_index = get_cache_index(pf_addr+stride*i,0); //choose a target cache entry
			pthread_mutex_unlock(&mapmutex);
			if(pf_index <= 0){
				endline = i;
				success = false;
				break;
			}

			indices[i]=pf_index;
			//lock cachelines
			//Make sure this entry is not contended, if this is the case likely it is used for other transfers, therefore we abort
			if(pthread_mutex_trylock(&cachemutex[pf_index%cachelocks]) != 0){ 
				endline = i;
				success = false;
				break;
			}
		}
		

		if(!success){//Dont bother prefetch if we have contention - fetch most likely already in place
#ifdef ARGO_PREFETCH_DEBUG
		printf("Aborting prefetch, lines under contention\n");
#endif
		//Unlock used cachelines
		for(i=0; i<endline; i++){
			pthread_mutex_unlock(&cachemutex[indices[endline-1-i]%cachelocks]);
		}
			free(indices);
			return;
		}

		for(i=0; i<endline; i++){
			stats.prefetchesissued++;
			long cacheindex = indices[i];
			unsigned long long tmptag = cachecontrol[cacheindex].tag;
			//If tags differ and the line is dirty we need to write it back before loading in something new
			if(tmptag != GLOBAL_NULL && tmptag != pf_addr+i*stride && cachecontrol[cacheindex].dirty==DIRTY){
				if(!mprotted){
					mprotted=true;
				}
				mprotect(global_base_addr+tmptag,pagesize*pageline,PROT_READ); //remove write permission before writing back, otherwise other threads might write these pages
				store_line_prefetch(cacheindex, tmptag, get_homenode(tmptag), get_offset(tmptag)); //Write back
			}
		}

		if(mprotted){
			end_prefetch_epoch(); //Sync accesses to MPI
			pthread_mutex_lock(&mapmutex);
			for(i=0; i<endline; i++){
				//				long cacheindex = pf_index+i;
				long cacheindex = indices[i];
				unsigned long long tmptag = pf_addr+i*stride;
				if(tmptag != GLOBAL_NULL){
					//Remove old mappings in cachemap 'directory', otherwise these will map to old indices resulting in ineffective replacement policy
					remove_cache_index(tmptag,cacheindex); 
				}
			}
			pthread_mutex_unlock(&mapmutex);

			//Fetch the lines
			for(i=0; i<endline; i++){
				long cacheindex = indices[i];
#ifdef ARGO_PREFETCH_DEBUG
				printf("ip prefetching :(%d,%d), stride:%ld, ip:%ld addr:%ld\n",cacheindex,indices[endline-1],stride,ip,addr);
#endif
				unsigned long long tmptag = pf_addr+i*stride;
				prefetch_line(cacheindex, tmptag, get_homenode(tmptag), get_offset(tmptag));
			}
			end_prefetch_epoch();

			//Map the data to local virtual memory
			double map1 = MPI_Wtime();
			for(i=0; i<endline; i++){
				long cacheindex = indices[i];
				vm::map_memory(global_base_addr+pf_addr+i*stride, pagesize*pageline, pagesize*pageline*cacheindex, PROT_READ);
			}
			double map2 = MPI_Wtime();
		}

		for(i=0; i<endline; i++){ //Unlock cachelines
			long cacheindex = indices[endline-1-i];
			pthread_mutex_unlock(&cachemutex[cacheindex%cachelocks]);
		}
		free(indices);
  }

  trackers[tracker_index].last_addr = addr;
  trackers[tracker_index].last_stride = stride;
  return;
}

extern char running;
#include "mpi.h"
void *prefetcher(void *x){
  UNUSED_PARAM(x);
  double t1,t2;
  while(running){
    sem_wait(&prefetchsem);
    //t1 = MPI_Wtime();
    if(miss_addr != 0 && miss_ip != 0){
				if(prefetch_policy==5){
					stream_plus(miss_addr);
				}
				else if(prefetch_policy==6){
					ip_prefetch(miss_addr,miss_ip);
				}
				else if(prefetch_policy==7){

				}
				else if(prefetch_policy==8){

				}
				else if(prefetch_policy==9){

				}
				else{
					printf("invalid prefetch policy:%d miss addr:%ld miss ip:%ld\n",prefetch_policy,miss_addr,miss_ip);
					return NULL;
				}
    }
  }
	return NULL;
}
