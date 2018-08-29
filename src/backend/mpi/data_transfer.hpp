#ifndef argo_data_transfer_hpp
#define argo_data_transfer_hpp argo_data_transfer_hpp

#include "data_distribution/data_distribution.hpp"
#include "swdsm.h"
#include "argo.hpp"


/*OPEN AND CLOSING CONNECTIONS*/
/**
 * @brief  Opens a window to the global address space
 * Opens a window to the global address space, currently always an exclusive right, this can be changed to allow overlapping accesses to same window
 * @param homenode what node we want to open a window to
 */
void start_epoch(unsigned long homenode);

/**
 * @brief Closes all windows
 *Closes all windows (that are open based on data_windows_used) to the global address space / synchronizes access to MPI
 */
void end_epoch();

/**
 * @brief Open prefetch window to global address space
 * Opens a 'prefetch' window to the global address space, currently always an exclusive right, this can be changed to allow overlapping accesses to same window. These prefetch windows can be used the same as normal data windows, but intentionally is thought of as being able to handle parallel requests to the global address space as they map to the same addresses. Use with care as data-races are undefined.
 * @param homenode what node we want to open a window to
 */
void start_prefetch_epoch(unsigned long homenode);

/**
 * @brief Closes all prefetch windows
 *Closes all prefetch windows (that are open based on prefetch_windows_used) to the global address space / synchronizes access to MPI
 */
void end_prefetch_epoch();


/*WRITING*/
/**
 * @brief stores a cacheline of pages remotely - only writing back what has been written locally since last synchronization point
 * @param cacheindex index in local page cache
 * @param line_offset address to page in global address space (offset to the VA of the buffer used for global addresses)
 * @param homenode which homenode data is on
 * @param offset offset of data on homenode
 */
void store_line(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long offset);
/**
 * @brief synchronizes data transfer to an MPI node. (Not sync as in coherence)
 */
void store_line_sync();
/**
 * @brief stores a page remotely - only writing back what has been written locally since last synchronization point, then synchronizes data transfer over MPI (Not sync as in coherence)
 * @param cacheindex index in local page cache
 * @param line_offset address to page in global address space (offset to the VA of the buffer used for global addresses)
 * @param homenode which homenode data is on
 * @param offset offset of data on homenode
 */
void store_line_and_sync(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long offset);



/*READING*/
/**
 * @brief Function for loading a line into global address space
 * @param cacheindex index in local page cache
 * @param line_offset address to page(cacheline) in global address space (offset to the VA of the buffer used for global addresses)
 * @param homenode which homenode data is on
 * @param offset offset of data on homenode
 */
void load_line(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long offset);
/**
 * @brief synchronizes data transfer to an MPI node. (Not sync as in coherence)
 */
void load_line_sync();

/**
 * @brief Function for loading a line into global address space using a separate window
 * Can be done asynchrounously and parallel to normal accesses with load line. In older versions of MPI this may not be supported however!
 * @param cacheindex index in local page cache
 * @param line_offset address to page(cacheline) in global address space (offset to the VA of the buffer used for global addresses)
 * @param homenode which homenode data is on
 * @param offset offset of data on homenode
 */

void load_line_and_sync(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long offset);

/*PREFETCH and parallel accesses*/
/*Functions that can be used to access address space parallell to normal cache misses*/
/**
 * @brief Function for loading a line into global address space using a separate window
 * Can be done asynchrounously and parallel to normal accesses with load line. In older versions of MPI this may not be supported however!
 * @param cacheindex index in local page cache
 * @param line_offset address to page(cacheline) in global address space (offset to the VA of the buffer used for global addresses)
 * @param homenode which homenode data is on
 * @param offset offset of data on homenode
 */
void prefetch_line(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long offset);

/**
 * @brief stores a cacheline of pages remotely - only writing back what has been written locally since last synchronization point, but doing this in a window separate from the normal MPI window used for cache misses. 
 *In order to fully understand what is going on, reading up on MPI windows and its memory model (But general rule is to just dont have data races on a page).
 * @param cacheindex index in local page cache
 * @param line_offset address to page in global address space (offset to the VA of the buffer used for global addresses)
 * @param homenode which homenode data is on
 * @param offset offset of data on homenode
 */
void store_line_prefetch(unsigned long cacheindex, unsigned long local_offset, unsigned long homenode, unsigned long offset);



/*EVICTIONS*/
/**
 *@brief Evicts lines from cacheindex to cacheindex+lines that doesnt correspond to starttag to starttag+lines
 *Evicts lines number of cachelines starting from cacheindex 'cacheindex'. 
 *Starting address we are populating in the cache is starttag, 
 *so make sure this is not evicted.
 *@param cacheindex cacheindex to start to evict from
 *@param lines number of lines
 *@param starttag tag we are loading into the cache in the future, dont evict addresses corresponding to this (but only if they are in incremental order)
 */
void evict_and_prepare_populate(unsigned long cacheindex, unsigned long lines, unsigned long starttag);

#endif
