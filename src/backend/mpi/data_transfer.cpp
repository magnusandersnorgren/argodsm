#include "virtual_memory/virtual_memory.hpp"
#include "data_transfer.hpp"
#include <atomic>

/*namespace*/
namespace vm = argo::virtual_memory;

/*stats*/
extern argo_statistics stats;
#ifdef VA_TRACE
extern unsigned int *VA_usectr;
#endif

/*pagecache*/
extern unsigned long cachesize;
extern control_data * cachecontrol;
extern char* cachedata;
#ifdef ARGO_DIFF
extern char* pagecopy;
#endif
/*common*/
extern char running;
static const unsigned long pagesize = 4096;
extern int numtasks;
extern unsigned long pageline;

/*address space*/
extern unsigned long size_of_all;
extern void* global_base_addr;
extern unsigned long size_of_chunk;
extern unsigned long GLOBAL_NULL;

/*windows*/
extern MPI_Win *global_address_window;
extern MPI_Win *parallel_global_address_window;
extern char * data_windows_used;
extern char * prefetch_windows_used;


void start_epoch(unsigned long homenode){
  if(data_windows_used[homenode] == 0){
		MPI_Win_lock(MPI_LOCK_EXCLUSIVE, homenode , 0, global_address_window[homenode]);
		data_windows_used[homenode] = 1;
  }
}

void end_epoch(){
  int i;
  for(i = 0; i < (unsigned long)numtasks; i++){
    if(data_windows_used[i] == 1){
      MPI_Win_unlock(i, global_address_window[i]);
      data_windows_used[i] = 0;
    }
  }
}

void start_prefetch_epoch(unsigned long homenode){
  if(prefetch_windows_used[homenode] == 0){
		MPI_Win_lock(MPI_LOCK_EXCLUSIVE, homenode , 0, parallel_global_address_window[homenode]);
		prefetch_windows_used[homenode] = 1;
  }
}

void end_prefetch_epoch(){
  int i;
  for(i = 0; i < (unsigned long)numtasks; i++){
    if(prefetch_windows_used[i] == 1){
      MPI_Win_unlock(i, parallel_global_address_window[i]);
      prefetch_windows_used[i] = 0;
    }
  }
}




/**brief
	 Updates sharing
*/
bool set_sharing(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long homenode_offset){

}

/**brief
Loads data into cache and make sure this is synchronized over MPI. (Not coherently with other nodes, rather, makes sure data is actually transfered after this function)
*/
void load_line_and_sync(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long homenode_offset){
  double t1,t2,w1,w2;//Timing purposes
	w1 = MPI_Wtime();
	int i;
  unsigned long id = 1 << argo_get_nid();
  unsigned long invid = ~id;
	assert(cacheindex <= cachesize);	

  void * lineptr = (char*)global_base_addr + local_offset;
  unsigned long size = pagesize*pageline;	
  bool stored = false;

	if(cachecontrol[cacheindex].tag != GLOBAL_NULL ){
		unsigned long tmptag = cachecontrol[cacheindex].tag;
		void * tmpptr = (char*)global_base_addr + tmptag;
		char dirty;
		dirty = cachecontrol[cacheindex].dirty;
		if(dirty == DIRTY && tmptag != local_offset && tmptag != GLOBAL_NULL){
			if(!stored){
				stored = true;
			}
			double map1 = MPI_Wtime();
			mprotect(tmpptr,pagesize*pageline,PROT_READ);
			double map2 = MPI_Wtime();
			stats.maptime += (map2-map1);
			store_line(cacheindex, tmptag, get_homenode(tmptag), get_offset(tmptag));
			stats.writebacks++;
		}
	}
  if(stored){
		store_line_sync();
  }
	w2 = MPI_Wtime();
	stats.writebacktime+=(w2-w1);
  w1 = MPI_Wtime();
  unsigned int evictctr=0;
  unsigned long evicttag = cachecontrol[cacheindex].tag;
  void * evictptr = (char*)global_base_addr + evicttag;
    if(cachecontrol[cacheindex].tag != GLOBAL_NULL){
      cachecontrol[cacheindex].tag = GLOBAL_NULL;
      cachecontrol[cacheindex].dirty = CLEAN;
      evictctr++;
    }
    else{
			double map1 = MPI_Wtime();
      mprotect(evictptr,pagesize*pageline*evictctr,PROT_NONE); //Finally invalidate this region, will trigger a segfault at future accesses
			double map2 = MPI_Wtime();
			stats.maptime += (map2-map1);
      stats.evictions+=(evictctr*pageline);
      evictctr=0;
      evicttag = cachecontrol[cacheindex].tag;
      evictptr = (char*)global_base_addr + evicttag;
    }   
		//}
  if(evictctr > 0){
		double map1 = MPI_Wtime();
		mprotect(evictptr,pagesize*pageline*evictctr,PROT_NONE);
		double map2 = MPI_Wtime();
		stats.maptime += (map2-map1);
		stats.evictions+=(evictctr*pageline);
  }
  w2 = MPI_Wtime();
  stats.evicttime+=(w2-w1);

  t1 = MPI_Wtime();
	MPI_Win_lock(MPI_LOCK_SHARED, homenode, 0, global_address_window[homenode]);
	MPI_Get(&cachedata[cacheindex*pagesize*pageline],
					size,
					MPI_BYTE,
					homenode,
					homenode_offset,
					size,
					MPI_BYTE,
					global_address_window[homenode]);
	MPI_Win_unlock(homenode, global_address_window[homenode]);
  t2 = MPI_Wtime();
  stats.loadtransfertime += (t2-t1);
  stats.loadtransferavg = (stats.loadtransferavg+(t2-t1))/2.0;

	
	cachecontrol[cacheindex].dirty = CLEAN;
	cachecontrol[cacheindex].tag = local_offset;

#ifdef CACHE_TRACE
		cachecontrol[cacheindex+i].usectr++;
#endif
#ifdef VA_TRACE
		VA_usectr[(local_offset/pagesize)+i]++;
#endif
  stats.loads+=pageline;
	return;
}


/** @brief Transfer data to another node*/
void store_line(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long homenode_offset){
  unsigned int i,j;
  int cnt = 0;
  bool empty = true;
  char * real = (char *)global_base_addr+local_offset;
	if(data_windows_used[homenode] == 0){
		MPI_Win_lock(MPI_LOCK_EXCLUSIVE, homenode, 0, global_address_window[homenode]);
    data_windows_used[homenode] = 1;
	}
		MPI_Put(&cachedata[cacheindex*pagesize*pageline],pagesize*pageline, MPI_BYTE, homenode, homenode_offset, pagesize*pageline, MPI_BYTE, global_address_window[homenode]);
#ifdef ARGO_DIFF
	unsigned char * copy = (unsigned char *)(pagecopy + cacheindex*pagesize*pageline);
  for(i = 0; i < pagesize; i+=DRF_UNIT){
#ifdef DEBUG
    printf("storing cacheindex byte:%d/%d \n",i,pagesize);
#endif
		int branchval;
		//Diffing assumes copy has a state of the page(s) that was snapshoted when moving from read access to read/write access
		for(j=i; j < i+DRF_UNIT; j++){
			branchval = real[j] != copy[j];
			if(branchval != 0){
				break;
			}
		}
		if(branchval != 0){ //if they are different, we should write it back, just keep going until we can write back a larger region
			cnt+=DRF_UNIT;
		}
		else{ //copy and real is equal, so just write back what we have until now
			if(cnt > 0){
				MPI_Put(&real[(i)-cnt] ,cnt, MPI_BYTE, homenode, homenode_offset+((i)-cnt), cnt, MPI_BYTE, global_address_window[homenode]);
				cnt = 0;
				empty = false;
			}
		}
	}
  //Put whatever is left
  if(cnt > 0){
    MPI_Put(&real[(i)-cnt] ,cnt, MPI_BYTE, homenode, homenode_offset+((i)-cnt), cnt, MPI_BYTE, global_address_window[homenode]);
    empty = false;
  }
  if(empty){
    stats.emptystores++;
  }
#endif

  stats.stores+=pageline;
}
/** @brief transfer data to another node using a separate address space in parallel to normal accesses*/
void store_line_prefetch(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long homenode_offset){
  unsigned int i,j;
  int cnt = 0;
  bool empty = true;
  char * real = (char *)global_base_addr+local_offset;
	if(prefetch_windows_used[homenode] == 0){
		MPI_Win_lock(MPI_LOCK_EXCLUSIVE, homenode, 0, parallel_global_address_window[homenode]);
    prefetch_windows_used[homenode] = 1;
	}

	MPI_Put(&cachedata[cacheindex*pagesize*pageline],pagesize*pageline, MPI_BYTE, homenode, homenode_offset, pagesize*pageline, MPI_BYTE, parallel_global_address_window[homenode]);

#ifdef ARGO_DIFF
	unsigned char * copy = (unsigned char *)(pagecopy + cacheindex*pagesize*pageline);
  for(i = 0; i < pagesize; i+=DRF_UNIT){
    //printf("storing cacheindex byte:%d/%d \n",i,pagesize);    
		int branchval;
		for(j=i; j < i+DRF_UNIT; j++){
			branchval = real[j] != copy[j];
			if(branchval != 0){
				break;
			}
		}
		if(branchval != 0){
			cnt+=DRF_UNIT;
		}
		else{
			if(cnt > 0){
				MPI_Put(&real[(i)-cnt] ,cnt, MPI_BYTE, homenode, homenode_offset+((i)-cnt), cnt, MPI_BYTE, global_address_window[homenode]);
				cnt = 0;
				empty = false;
			}
		}
	}

  //Put whatever is left
  if(cnt > 0){
    MPI_Put(&real[(i)-cnt] ,cnt, MPI_BYTE, homenode, homenode_offset+((i)-cnt), cnt, MPI_BYTE, global_address_window[homenode]);
    empty = false;
  }
  if(empty){
    stats.emptystores++;
  }
#endif

  stats.stores+=pageline;
}
/** @brief store data and sync transfer to MPI*/
void store_line_and_sync(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long homenode_offset){
  store_line(cacheindex,local_offset,homenode,homenode_offset);
  store_line_sync();
}
/** @brief sync transfer to MPI*/
void store_line_sync(){
	end_epoch();
}



//evicts lines [cacheindex,cacheindex+lines]
/**@brief
 *evicts lines number of cachelines starting from cacheindex 'cacheindex'. 
 *Starting address we are populating in the cache is starttag, 
 *so make sure this is not evicted.
 */
void evict_and_prepare_populate(unsigned long cacheindex, unsigned long lines, unsigned long starttag){
  double w1,w2;  
  w1 = MPI_Wtime();

  unsigned int evictctr=0;
  unsigned long evicttag = cachecontrol[cacheindex].tag;
  void * evictptr = (char*)global_base_addr + evicttag; //Startptr -SI large regions
  
  for(int i = 0; i<lines; i++){
		//If this is not a wanted address (or null) evict
    if(starttag+i*pagesize != cachecontrol[cacheindex+i].tag && cachecontrol[cacheindex+i].tag != GLOBAL_NULL){
      cachecontrol[cacheindex+i].tag = GLOBAL_NULL;
      cachecontrol[cacheindex+i].dirty = CLEAN;
			/*
				We increase a counter so we can later on,
				if the full area is not contiguous,
				we can map larger areas reducing number of slow syscalls
			*/
      evictctr++;
    }
    else{//Found an address that is not evicted / already correct addres
			if(evictctr > 0){
				double map1 = MPI_Wtime();
				mprotect(evictptr,pagesize*evictctr,PROT_NONE); // evict addresses so far
				double map2 = MPI_Wtime();
				stats.maptime += (map2-map1);
				stats.evictions+=evictctr;
				evictctr=0;
			}
			//Set new ptr to evict from, and what next tag should be
      evicttag = cachecontrol[cacheindex+i].tag;
      evictptr = (char*)global_base_addr + evicttag;
    }   
  }

	//All done, evict what is left
  if(evictctr > 0){
		double map1 = MPI_Wtime();
    mprotect(evictptr,pagesize*evictctr,PROT_NONE); //Makes sure future accesses will cause segfault
		double map2 = MPI_Wtime();
		stats.maptime += (map2-map1);
    stats.evictions+=evictctr; //Number of evictions
  }
  w2 = MPI_Wtime();
  stats.evicttime+=(w2-w1);

}


void load_line(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long homenode_offset){
  void * lineptr = (char*)global_base_addr + local_offset;
	start_epoch(homenode);
	//Args(cacheaddr, sizeofline, unit moved over MPI, homenode to load from, offset from target window base, size, unit, window used)
	MPI_Get(&cachedata[cacheindex*pagesize*pageline],
					pagesize*pageline,
					MPI_BYTE,
					homenode,
					homenode_offset,
					pagesize*pageline,
					MPI_BYTE,
					global_address_window[homenode]);

	//Set cache tag and dirty bit (For scaling memory we can set this to dirty immediately and later on map it with RW access rights. May want to update sharing here
	cachecontrol[cacheindex].dirty = CLEAN;
	cachecontrol[cacheindex].tag = local_offset;

#ifdef CACHE_TRACE
	//try to trace what indices are used at what frequency
	cachecontrol[cacheindex].usectr++;
#endif

#ifdef VA_TRACE
	//try to trace what adresses are used at what frequency
	VA_usectr[(local_offset/pagesize)]++;
#endif
  stats.loads++;
}

void prefetch_line(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long homenode_offset){
  void * lineptr = (char*)global_base_addr + local_offset;
	start_prefetch_epoch(homenode);
	MPI_Get(&cachedata[cacheindex*pagesize*pageline],
					pagesize*pageline,
					MPI_BYTE,
					homenode,
					homenode_offset,
					pagesize*pageline,
					MPI_BYTE,
					parallel_global_address_window[homenode]);
	//Set cache tag and dirty bit (For scaling memory we can set this to dirty immediately and later on map it with RW access rights. May want to update sharing here
	cachecontrol[cacheindex].dirty = CLEAN;
	cachecontrol[cacheindex].tag = local_offset;

#ifdef CACHE_TRACE
	//try to trace what indices are used at what frequency
	cachecontrol[cacheindex].usectr++;
#endif

#ifdef VA_TRACE
	//try to trace what adresses are used at what frequency
	VA_usectr[(local_offset/pagesize)]++;
#endif
  stats.loads++;
}
