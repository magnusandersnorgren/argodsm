/**
 * @file
 * @brief This file provides the dynamic allocator for ArgoDSM
 * @copyright Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.
 */

#ifndef argo_dynamic_allocators_h
#define argo_dynamic_allocators_h argo_dynamic_allocators_h

/**
 * @brief dynamic allocation function, for C interface
 * @param size number of bytes to allocate
 * @return pointer to the newly allocated memory
 */
void* dynamic_alloc(size_t size, size_t alignment=32);

/**
 * @brief basic free function for dynamic allocations, for C interface
 * @param ptr pointer to free
 */
void dynamic_free(void* ptr);

#endif /* argo_dynamic_allocators_h */
