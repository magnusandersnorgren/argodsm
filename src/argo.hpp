/**
 * @file
 * @brief This file provides the full C++ interface to ArgoDSM
 * @copyright Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.
 */

#ifndef argo_argo_hpp
#define argo_argo_hpp argo_argo_hpp

#include "allocators/allocators.hpp"
#include "backend/backend.hpp"
#include "types/types.hpp"
#include "synchronization/synchronization.hpp"

extern "C" {
#include "argo.h"
}

/**
 * @brief ArgoDSM namespace
 */
namespace argo {
	/**
	 * @brief namespace for different memory pools
	 * @todo move Documentation to appropriate file
	 */
	namespace mempools { }

	/**
	 * @brief namespace for global locks, i.e locks that work across multiple ArgoDSM nodes
	 * @todo move Documentation to appropriate file
	 */
	namespace globallock { }

	/**
	 * @brief namespace for data distribution modules
	 * @todo move Documentation to appropriate file
	 */
	namespace data_distribution { }

	/**
	 * @brief initialize ArgoDSM system
	 * @param size the desired size of the global memory in bytes
	 */
	void init(size_t size);

	/**
	 * @brief shut down ArgoDSM system
	 */
	void finalize();

	/**
	 * @brief A unique ArgoDSM node identifier. Counting starts from 0.
	 * @return The node ID
	 */
	int node_id();
	/**
	 * @brief Number of ArgoDSM nodes being run
	 * @return The total number of ArgoDSM nodes
	 */
	int number_of_nodes();
	double get_taskavg();
	int get_sched_policy();
	double get_signalavg();
	int in_cache(void* ptr, size_t size);
	void clear_state();
} // namespace argo

#endif /* argo_argo_hpp */
