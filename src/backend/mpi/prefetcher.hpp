#ifndef argo_prefetcher_hpp
#define argo_prefetcher_hpp argo_prefetcher_hpp

#include "swdsm.h"
#include "data_transfer.hpp"
#include "virtual_memory/virtual_memory.hpp"
#include <cmath>

void init_prefetcher();
void simple_line_prefetch();
void stream_prefetch();
void markov_prefetch();
void ip_prefetch();
void *prefetcher(void *x);

//#ifndef LOOKAHEAD
//#define LOOKAHEAD 0
//#endif

#define PREDICTIONS 4
#define STREAM_CONFIDENT 2
#define AMPM_ZONES 16
#define AMPM_PAGES_IN_ZONE 128
#define IP_TRACKER_COUNT 1024

#endif
