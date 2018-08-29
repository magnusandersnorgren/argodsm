/**
 * @file
 * @brief MPI backend implemenation
 * @copyright Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.
 */

#include "../backend.hpp"

#include <atomic>
#include <type_traits>

#include "mpi.h"
#include "swdsm.h"

/**
 * @brief MPI communicator for node processes
 * @deprecated prototype implementation detail
 * @see swdsm.h
 * @see swdsm.cpp
 */

/**
 * @todo MPI communication channel for exclusive accesses
 * @deprecated prototype implementation detail
 * @see swdsm.h
 * @see swdsm.cpp
 */
extern MPI_Win *global_address_window;

/**
 * @todo should be changed to qd-locking (but need to be replaced in the other files as well)
 *       or removed when infiniband/the mpi implementations allows for multithreaded accesses to the interconnect
 * @deprecated prototype implementation detail
 */
extern pthread_mutex_t window_mutex;
extern bool swdsm_is_loaded;
static bool mpib_is_loaded = false;
static void mpib_loaded()  __attribute__((constructor));
void mpib_loaded() {
	mpib_is_loaded = true;
}
/**
 * @brief Returns an MPI integer type that exactly matches in size the argument given
 *
 * @param size The size of the datatype to be returned
 * @return An MPI datatype with MPI_Type_size == size
 */
static MPI_Datatype fitting_mpi_int(std::size_t size) {
	MPI_Datatype t_type;
	using namespace argo;

	switch (size) {
	case 1:
		t_type = MPI_INT8_T;
		break;
	case 2:
		t_type = MPI_INT16_T;
		break;
	case 4:
		t_type = MPI_INT32_T;
		break;
	case 8:
		t_type = MPI_INT64_T;
		break;
	default:
		throw std::invalid_argument(
			"Invalid size (must be either 1, 2, 4 or 8)");
		break;
	}

	return t_type;
}

/**
 * @brief Returns an MPI unsigned integer type that exactly matches in size the argument given
 *
 * @param size The size of the datatype to be returned
 * @return An MPI datatype with MPI_Type_size == size
 */
static MPI_Datatype fitting_mpi_uint(std::size_t size) {
	MPI_Datatype t_type;
	using namespace argo;

	switch (size) {
	case 1:
		t_type = MPI_UINT8_T;
		break;
	case 2:
		t_type = MPI_UINT16_T;
		break;
	case 4:
		t_type = MPI_UINT32_T;
		break;
	case 8:
		t_type = MPI_UINT64_T;
		break;
	default:
		throw std::invalid_argument(
			"Invalid size (must be either 1, 2, 4 or 8)");
		break;
	}

	return t_type;
}

/**
 * @brief Returns an MPI floating point type that exactly matches in size the argument given
 *
 * @param size The size of the datatype to be returned
 * @return An MPI datatype with MPI_Type_size == size
 */
static MPI_Datatype fitting_mpi_float(std::size_t size) {
	MPI_Datatype t_type;
	using namespace argo;

	switch (size) {
	case 4:
		t_type = MPI_FLOAT;
		break;
	case 8:
		t_type = MPI_DOUBLE;
		break;
	case 16:
		t_type = MPI_LONG_DOUBLE;
		break;
	default:
		throw std::invalid_argument(
			"Invalid size (must be power either 4, 8 or 16)");
		break;
	}

	return t_type;
}

namespace argo {
	namespace backend {
		bool is_loaded() {
			return mpib_is_loaded && swdsm_is_loaded;
		}
		void init(std::size_t size) {
			argo_initialize(size);
		}

		node_id_t node_id() {
			return argo_get_nid();
		}

		int number_of_nodes() {
			return argo_get_nodes();
		}

		char* global_base() {
			return static_cast<char*>(argo_get_global_base());
		}

		std::size_t global_size() {
			return argo_get_global_size();
		}

		void finalize() {
			argo_finalize();
		}

		void barrier(std::size_t tc) {
			swdsm_argo_barrier(tc);
		}
		void clear_state(){
			return argo_clear_state();
		}

		int in_cache(void* ptr, size_t size){
			return argo_in_cache(ptr,size);
		}
		int get_sched_policy(){
			return argo_get_sched_policy();
		}
		double get_taskavg(){
			return argo_get_taskavg();
		}
		double get_signalavg(){
			return argo_get_signalavg();
		}
		template<typename T>
		void broadcast(node_id_t source, T* ptr) {
			pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);
			MPI_Bcast(static_cast<void*>(ptr), sizeof(T), MPI_BYTE, source, MPI_COMM_WORLD);
			pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);
		}
		void acquire() {
			argo_acquire();
			std::atomic_thread_fence(std::memory_order_acquire);
		}
		void release() {
			std::atomic_thread_fence(std::memory_order_release);
			argo_release();
		}

#include "../explicit_instantiations.inc.cpp"
		namespace atomic {
			void _exchange(global_ptr<void> obj, void* desired,
					std::size_t size, void* output_buffer) {
				pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);
				MPI_Datatype t_type = fitting_mpi_int(size);
				// Perform the exchange operation
				//				printf("atomic exc:%d\n", obj.node());
				MPI_Win_lock(MPI_LOCK_EXCLUSIVE, obj.node(), 0, global_address_window[0]);
				MPI_Fetch_and_op(desired, output_buffer, t_type, obj.node(), obj.offset(), MPI_REPLACE, global_address_window[0]);
				MPI_Win_unlock(obj.node(), global_address_window[0]);
				// Cleanup
				pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);
			}

			void _store(global_ptr<void> obj, void* desired, std::size_t size) {
				pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);
				MPI_Datatype t_type = fitting_mpi_int(size);
				// Perform the store operation
				//				printf("atomic str\n");
				MPI_Win_lock(MPI_LOCK_EXCLUSIVE, obj.node(), 0, global_address_window[0]);
				MPI_Put(desired, 1, t_type, obj.node(), obj.offset(), 1, t_type, global_address_window[0]);
				MPI_Win_unlock(obj.node(), global_address_window[0]);
				// Cleanup
				pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);
			}

			void _load(global_ptr<void> obj, std::size_t size,
					void* output_buffer) {
				pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);
				MPI_Datatype t_type = fitting_mpi_int(size);
				// Perform the store operation
				//printf("atomic ld\n");
				MPI_Win_lock(MPI_LOCK_SHARED, obj.node(), 0, global_address_window[0]);
				MPI_Get(output_buffer, 1, t_type, obj.node(), obj.offset(), 1, t_type, global_address_window[0]);
				MPI_Win_unlock(obj.node(), global_address_window[0]);
				// Cleanup
				pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);
			}

			void _compare_exchange(global_ptr<void> obj, void* desired,
					std::size_t size, void* expected, void* output_buffer) {
				pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);
				MPI_Datatype t_type = fitting_mpi_int(size);
				// Perform the store operation
				//printf("atomic compexc\n");
				MPI_Win_lock(MPI_LOCK_EXCLUSIVE, obj.node(), 0, global_address_window[0]);
				MPI_Compare_and_swap(desired, expected, output_buffer, t_type, obj.node(), obj.offset(), global_address_window[0]);
				MPI_Win_unlock(obj.node(), global_address_window[0]);
				// Cleanup
				pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);
			}

			/**
			 * @brief Atomic fetch&add for the MPI backend (for internal usage)
			 *
			 * This function requires the correct MPI type to work. Usually, you
			 * want to use the three _fetch_add_{int,uint,float} functions
			 * instead, which determine the correct type themselves and then
			 * call this function
			 *
			 * @param obj The pointer to the memory location to modify
			 * @param value Pointer to the value to add
			 * @param t_type MPI type of the object, value, and output buffer
			 * @param output_buffer Location to store the return value
			 */
			void _fetch_add(global_ptr<void> obj, void* value,
					MPI_Datatype t_type, void* output_buffer) {
				pthread_mutex_lock(&window_mutex);//sem_wait(&ibsem);
				// Perform the exchange operation
				//printf("atomic fadd\n");
				MPI_Win_lock(MPI_LOCK_EXCLUSIVE, obj.node(), 0, global_address_window[0]);
				MPI_Fetch_and_op(value, output_buffer, t_type, obj.node(), obj.offset(), MPI_SUM, global_address_window[0]);
				MPI_Win_unlock(obj.node(), global_address_window[0]);
				// Cleanup
				pthread_mutex_unlock(&window_mutex); //sem_post(&ibsem);
			}

			void _fetch_add_int(global_ptr<void> obj, void* value,
					std::size_t size, void* output_buffer) {
				MPI_Datatype t_type = fitting_mpi_int(size);
				_fetch_add(obj, value, t_type, output_buffer);
			}

			void _fetch_add_uint(global_ptr<void> obj, void* value,
					std::size_t size, void* output_buffer) {
				MPI_Datatype t_type = fitting_mpi_uint(size);
				_fetch_add(obj, value, t_type, output_buffer);
			}

			void _fetch_add_float(global_ptr<void> obj, void* value,
					std::size_t size, void* output_buffer) {
				MPI_Datatype t_type = fitting_mpi_float(size);
				_fetch_add(obj, value, t_type, output_buffer);
			}
		} // namespace atomic
	} // namespace backend
} // namespace argo
