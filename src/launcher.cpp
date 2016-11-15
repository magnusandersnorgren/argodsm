#include <iostream>
#include <type_traits>
#include <signal.h>
#include <stdio.h>
#include <dlfcn.h>

#include <unistd.h>

#include<atomic>
#include "argo.hpp"

#include <semaphore.h>

static bool is_loaded = false;
static bool is_initialized = false;
static bool is_initialized2 = false;
static thread_local bool is_argo_code = false;
static bool is_capturing = false;
//static bool segfault_siginfo;
static struct sigaction segfault_sigaction;
static struct sigaction segfault_sigaction2;
//static void (*segfault_sigaction)(int, siginfo_t*, void*);
//static void (*segfault_handler)(int);
argo::data_distribution::global_ptr<bool> argo_alive;

namespace aa = argo::allocators;
extern aa::default_dynamic_allocator_t aa::default_dynamic_allocator;

template<size_t SIZE>
	class memory_pool {
		private:
			/** @todo Documentation */
			static thread_local char memory[SIZE];
			/** @todo Documentation */
			static thread_local std::size_t max_size;
			/** @todo Documentation */
			static thread_local std::size_t offset;
			static thread_local int allocation_counter;
		public:
			/** type of allocation failures within this memory pool */
			using bad_alloc = std::bad_alloc;

			/**
			 * @brief  Resets the memory pool to the initial state instead of de-allocating and (re)allocating all buffers again.
			 * @warning do not use, it is not correct
			 */
			static void reset() {
				//if(allocation_counter != 0) {
				//	printf("allocs: %d\n", allocation_counter);
				//	std::cerr << "temporary allocation was not returned, but is now invalid!\n";
				//	argo::finalize();
				//}
				offset = 0;
			}
			/**
			 * @brief Reserve more memory
			 * @param size Amount of memory reserved
			 * @return The pointer to the first byte of the newly reserved memory area
			 * @todo move size check to separate function?
			 */
			static char* reserve(std::size_t size) {
				if(offset+size > max_size) throw bad_alloc();
				char* ptr = &memory[offset];
				offset += size;
				allocation_counter++;
				return ptr;
			}
			
			static void unreserve() {
				allocation_counter--;
			}

			static std::size_t available() {
				return max_size - offset;
			}

			static bool is_here(void* ptr) {
				return ptr >= static_cast<void*>(&memory[0]) && ptr <= static_cast<void*>(&memory[SIZE]);
			}
			static void print() {
				printf("MEMPOOL IS %p TO %p\n", &memory[0], &memory[SIZE]);
			}
	};
template<size_t S> thread_local char memory_pool<S>::memory[S];
template<size_t S> thread_local std::size_t memory_pool<S>::max_size = S;
template<size_t S> thread_local std::size_t memory_pool<S>::offset = 0;
template<size_t S> thread_local int memory_pool<S>::allocation_counter = 0;

typedef memory_pool<8096> static_mp;

//using capture_allocator_t = aa::generic_allocator<char, memory_pool<16*4096>, aa::null_lock>;


extern argo::mempools::global_memory_pool<>* default_global_mempool;
static void capture_reset() {
	static_mp::reset();
	//capture_allocator = capture_allocator_t(&static_mp);
}

static void loaded()  __attribute__((constructor));
void loaded() {
	//auto handle = dlopen("libargo.so", RTLD_NOW|RTLD_GLOBAL);
	//handle = dlopen("libargobackend-mpi.so", RTLD_NOW|RTLD_GLOBAL);
	//(void)handle;
	is_loaded = true;
}
static void launcher_down()  __attribute__((destructor));
void launcher_down() {
	is_loaded = false;
	is_argo_code = true;
	argo::finalize();
	printf("really, all is done, terminate now\n");
	_exit(0);
}

bool ready() {
	return is_loaded && argo_is_ready();
}

void launcher_handler(int sig, siginfo_t* si, void* unused) {
	bool was_argo_code = is_argo_code;
	is_argo_code = true;
	//printf("this is a problem %d, %p, %p, %p\n", sig, si, unused, si->si_addr);
	if(segfault_sigaction.sa_flags & SA_SIGINFO) {
		//printf("this is SIGINFO OLD\n");
		segfault_sigaction.sa_sigaction(sig, si, unused);
	} else {
		//printf("this is SIGHANDLER OLD\n");
		segfault_sigaction.sa_handler(sig);
	}
	is_argo_code = was_argo_code;
}

static void launcher_init() {
	if(is_initialized) return;
	is_initialized = true;
	std::cout << "init argo" << std::endl;
	static_mp::print();
	bool was_argo_code = is_argo_code;
	is_argo_code = true;
	argo::init(10l*1024l*1024l*1024l); // TODO make this runtime choosable
	//argo_alive = argo::conew_<bool>(true);
	//printf("mempool sanity: %p -> %p\n", default_global_mempool, default_global_mempool->memory);
	std::cout << "inited argo" << std::endl;
	struct sigaction argosig;
	printf("launcher sigaction %p\n", &argosig);
	sigaction(SIGSEGV, NULL, &argosig);
	segfault_sigaction = argosig;
	argosig.sa_sigaction = launcher_handler;
	argosig.sa_flags = SA_SIGINFO;
	printf("launcher sigaction2 %p\n", &argosig);
	sigaction(SIGSEGV, &argosig, &segfault_sigaction);
	printf("set launcher segfault handler\n");
	is_argo_code = was_argo_code;
	is_initialized2 = true;
	printf("application ArgoDSM node ID %d out of %d ArgoDSM nodes\n", argo::backend::node_id(), argo::backend::number_of_nodes());
}

template<typename R, typename... Ps>
class real_fn {
	const char* fname;
	R (*real_f)(Ps... ps);
	R (*argo_f)(Ps... ps);
	
	public:
		real_fn() : fname(nullptr), real_f(nullptr), argo_f(nullptr) {}
		real_fn(const char* name, R(*argo_fn)(Ps...)) : fname(name), real_f(nullptr), argo_f(argo_fn) {}
		R operator()(Ps... ps) {
			bool yes = ready();
			if(real_f == nullptr) {
				is_capturing = true;
				real_f = reinterpret_cast<R (*)(Ps...)>(dlsym(RTLD_NEXT, fname));
				//if(real_f == nullptr) {
				//	fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
				//}
				is_capturing = false;
				capture_reset();
			}

			if(yes && !is_initialized) {
					bool was = is_argo_code;
					is_argo_code = true;
					launcher_init();
					is_argo_code = was;
			}
			//printf("calling %s\n", fname);
			if(is_argo_code || !yes || !is_initialized2 || argo_f == nullptr) {
				R r = real_f(ps...);
				return r;
			} else {
				is_argo_code=true;
				//printf("calling argo for %s\n", fname);
				R r = argo_f(ps...);
				is_argo_code=false;
				return r;
			}
		}
};
template<typename... Ps>
class real_fn<void, Ps...> {
	const char* fname;
	using R = void;
	R (*real_f)(Ps... ps);
	R (*argo_f)(Ps... ps);
	
	public:
		real_fn(const char* name, R(*argo_fn)(Ps...)) : fname(name), real_f(nullptr), argo_f(argo_fn) {}
		void operator()(Ps... ps) {
			bool yes = ready();
			if(real_f == nullptr) {
				is_capturing = true;
				real_f = reinterpret_cast<R (*)(Ps...)>(dlsym(RTLD_NEXT, fname));
				is_capturing = false;
				capture_reset();
				//if(real_f == nullptr) {
				//	fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
				//}
				if(yes && !is_initialized) {
					bool was = is_argo_code;
					is_argo_code = true;
					launcher_init();
					is_argo_code = was;
				}
			}
			//printf("calling %s\n", fname);
			if(is_argo_code || !yes || !is_initialized || argo_f == nullptr) {
				real_f(ps...);
			} else {
				is_argo_code=true;
				//printf("calling argo_v for %s\n", fname);
				argo_f(ps...);
				is_argo_code=false;
			}
		}

};

void* argo_launcher_calloc(size_t nmemb, size_t size) {
	void* nptr = dynamic_alloc(nmemb*size);
	//std::fill(static_cast<char*>(nptr), static_cast<char*>(nptr)+(nmemb*size)+1, 0);
	std::fill(static_cast<char*>(nptr), static_cast<char*>(nptr)+(nmemb*size), 0);
	return nptr;
}

void* argo_launcher_realloc(void* ptr, size_t size) {
	void* nptr = dynamic_alloc(size);
	if(ptr == nullptr) return nptr;
	size_t oldsize = aa::default_dynamic_allocator.allocated_space(static_cast<char*>(ptr));
	size_t cpsize = (oldsize<size)?oldsize:size;
	std::copy(static_cast<char*>(ptr), static_cast<char*>(ptr)+cpsize, static_cast<char*>(nptr));
	dynamic_free(ptr);
	return nptr;
}

void* argo_launcher_mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
	(void)addr;
	(void)prot;
	(void)fd;
	(void)offset;
	if(flags & MAP_FIXED) {
		std::cerr << "MAP_FIXED in mmap() is not supported!\n";
		//argo::barrier();
		argo::finalize();
	}
	char* r = static_cast<char*>(dynamic_alloc(length, 4096));
	std::fill(r, r+length, 0);
	mprotect(addr, length, prot);
	return static_cast<void*>(r);
}

int argo_launcher_munmap(void* addr, size_t length) {
	(void)length;
	(void)addr;
	//dynamic_free(addr);
	return 0;
}

int argo_launcher_mprotect(void* addr, size_t len, int prot) {
	(void)addr;
	(void)len;
	(void)prot;
	std::cerr << "WARNING: mprotect is not supported!\n";
	//argo::barrier();
	//argo::finalize();
	//return -1;
	return 0;
}

pid_t argo_launcher_fork() {
	std::cerr << "fork() is not supported!\n";
	//argo::barrier();
	argo::finalize();
	exit(-1);
	return -1;
}

ssize_t argo_launcher_read(int fd, void *buf, size_t count) {
	auto was = is_argo_code;
	is_argo_code = true;
	if(default_global_mempool->is_inside(buf)) {
		auto r = read(fd, buf, count);
		is_argo_code = was;
		return r;
	}
	static const int bufsize = 16384;
	static thread_local char lbuf[bufsize];
	auto toread = bufsize<count?bufsize:count;
	auto r = read(fd, lbuf, toread);
	std::copy(lbuf, lbuf+toread, static_cast<char*>(buf));
	if(count == toread) {
		return r;
	} else {
		is_argo_code = was;
		return read(fd, static_cast<char*>(buf)+toread, count-toread);
	}
}

void* argo_launcher_memalign(size_t alignment, size_t size) {
	//printf("CALLING MEMALIGN!!!!!\n");
	char* p = static_cast<char*>(dynamic_alloc(size, alignment));
	//std::fill(p, p+size, 0);
	return p;
}
int argo_launcher_posix_memalign(void **memptr, size_t alignment, size_t size) {
	//printf("ARGO CALLING POSIX_MEMALIGN!!!!!\n");
	char* p = static_cast<char*>(dynamic_alloc(size, alignment));
	//std::fill(p, p+size, 0);
	*memptr = p;
	return 0;
}


struct argo_l_arg {
	void *(*start_routine) (void *);
	void *arg;
};
void* argo_launcher_thread_launcher(void* a) {
	printf("create thread...");
	if(!is_initialized2) {
		printf(" this must be a backend thread");
		is_argo_code = true;
	}
	auto arg = static_cast<argo_l_arg*>(a);
	argo_l_arg args = *arg;
	delete arg;
	printf(".. created thread now.\n");
	return args.start_routine(args.arg);
}

extern struct sigaction old_struct_sigaction;
extern "C" {
	void* dyn_alloc(size_t size) {
		return dynamic_alloc(size, 32);
	}
	void* malloc(size_t size) {
		if(is_capturing) {
			return static_cast<void*>(static_mp::reserve(size));
		}
		static real_fn<void*, size_t> f("malloc", dyn_alloc);
		//return f(size);
		auto retval = f(size);
		//std::fill(static_cast<char*>(retval), static_cast<char*>(retval)+(size), 0);
		return retval;
	}
	void free(void* ptr) {
		if(ptr == nullptr) return;
		if(static_mp::is_here(ptr)) {
			//static_mp::unreserve();
			return;
		}
		bool was = is_argo_code;
		if(is_initialized2 && default_global_mempool->is_inside(ptr)) {
			is_argo_code = true;
			dynamic_free(ptr);
			is_argo_code = was;
		} else {
			static real_fn<void, void*> f("free", nullptr);
			is_argo_code = true;
			f(ptr);
			is_argo_code = was;
		}
	}
	void* calloc(size_t nmemb, size_t size) {
		if(is_capturing) {
			char* nptr = static_mp::reserve(size);
			//std::fill(static_cast<char*>(nptr), static_cast<char*>(nptr)+(nmemb*size)+1, 0);
			std::fill(static_cast<char*>(nptr), static_cast<char*>(nptr)+(nmemb*size), 0);
			return nptr;
		}
		static real_fn<void*, size_t, size_t> f("calloc", argo_launcher_calloc);
		return f(nmemb, size);

	}
	
	void* realloc(void* ptr, size_t size) {
		static real_fn<void*, void*, size_t> f("realloc", argo_launcher_realloc);
		return f(ptr, size);
	}

	void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
		bool was = is_argo_code;
		if(fd != -1) {
			is_argo_code = true;
		}
		static real_fn<void*, void*, size_t, int, int, int, off_t> f("mmap", argo_launcher_mmap);
		auto rval = f(addr, length, prot, flags, fd, offset);
		if(fd != -1) {
			is_argo_code = was;
		}
		return rval;
	}

	int munmap(void* addr, size_t length) {
		//printf("munmap %p\n", addr);
		static real_fn<int, void*, size_t> f("munmap", argo_launcher_munmap);
		return f(addr, length);
		//(void)length;
		//return 0;
	}
	void *mremap(void *old_address, size_t old_size,  size_t new_size, int flags, ... /* void *new_address */) {
		if(is_initialized2 && default_global_mempool->is_inside(old_address)) {
			std::cerr << "mremap is not supported!\n";
			//argo::barrier();
			argo::finalize();
			exit(-1);
			return nullptr;
		}
		(void)old_size;
		(void)new_size;
		(void)flags;
		printf("mremap currently does not work!\n");
		return nullptr;
	}
	void *memalign(size_t alignment, size_t size) {
		static real_fn<void*, size_t, size_t> f("memalign", argo_launcher_memalign);
		return f(alignment, size);

	}
	void *aligned_alloc(size_t alignment, size_t size) {
		printf("CALLING ALIGNED_ALLOC!\n");
		static real_fn<void*, size_t, size_t> f("aligned_alloc", argo_launcher_memalign);
		return f(alignment, size);
	}
	int posix_memalign(void **memptr, size_t alignment, size_t size) {
		static real_fn<int, void**, size_t, size_t> f("posix_memalign", argo_launcher_posix_memalign);
		return f(memptr, alignment, size);
	}
	void *valloc(size_t size) {
		printf("CALLING VALLOC!\n");
		static real_fn<void*, size_t> f("valloc", dyn_alloc);
		return f(size);
	}
	void *pvalloc(size_t size) {
		printf("CALLING PVALLOC!\n");
		static real_fn<void*, size_t> f("pvalloc", dyn_alloc);
		return f(size);
	}
	int mprotect(void* addr, size_t len, int prot) {
		static real_fn<int, void*, size_t, int> f("mprotect", argo_launcher_mprotect);
		bool was = is_argo_code;
		if(is_initialized2 && !default_global_mempool->is_inside(addr) && !default_global_mempool->is_inside(static_cast<char*>(addr)+len)) {
			is_argo_code = true;
		}
		auto r = f(addr, len, prot);
		is_argo_code = was;
		return r;
		//return f(addr, len, prot);
	}
	int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg) {
		argo_l_arg* args = new argo_l_arg;
		args->start_routine = start_routine;
		args->arg = arg;
		static real_fn<int, pthread_t*, const pthread_attr_t*, void*(*)(void*), void*> f("pthread_create", nullptr);
		return f(thread, attr, &argo_launcher_thread_launcher, static_cast<void*>(args)); 
	}
	//pid_t fork() {
	//	static real_fn<pid_t> f("fork", argo_launcher_fork);
	//	return f();
	//}

/*	int open(const char *pathname, int flags, mode_t mode) {
		static real_fn<int, const char*, int, mode_t> f("open", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(pathname, flags, mode);
		is_argo_code = was;
		return retval;
	}
	int openat(int dirfd, const char *pathname, int flags, mode_t mode) {
		static real_fn<int, int, const char*, int, mode_t> f("openat", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(dirfd, pathname, flags, mode);
		is_argo_code = was;
		return retval;
	}
*/
	ssize_t read(int fd, void *buf, size_t count) {
		//printf("calling read\n");
		static real_fn<ssize_t, int, void*, size_t> f("read", nullptr);
		auto was = is_argo_code;
		is_argo_code = true;
		if(is_capturing || !is_initialized2 || !default_global_mempool->is_inside(buf)) {
			//printf("not argo-buf -> calling default read\n");
			auto r = f(fd, buf, count);
			is_argo_code = was;
			return r;
		}
		//printf("argo-buf -> processing\n");
		static const int bufsize = 16384;
		static thread_local char lbuf[bufsize];
		auto toread = (bufsize<count)?bufsize:count;
		//printf("argo-buf -> reading up to %ld\n", toread);
		auto r = f(fd, lbuf, toread);
		//printf("argo-buf -> reading %ld\n", r);
		is_argo_code = was;
		if(r < 0) {
			is_argo_code = was;
			return r;
		}
		std::copy(lbuf, lbuf+r, static_cast<char*>(buf));
		return r;
//		if(count == toread || r < toread) {
//			return r;
//		} else {
//			return r + read(fd, static_cast<char*>(buf)+toread, count-r);
//		}
	}
	size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
		//printf("calling fread, %p for %lu * %lu \n", ptr, size, nmemb);
		static real_fn<size_t, void*, size_t, size_t, FILE*> f("fread", nullptr);
		auto was = is_argo_code;
		is_argo_code = true;
		if(is_capturing || !is_initialized2 || !default_global_mempool->is_inside(ptr)) {
			//printf("not argo-buf -> calling default fread\n");
			auto r = f(ptr, size, nmemb, stream);
			is_argo_code = was;
			return r;
		}
		//printf("argo-buf -> fprocessing sz %ld\n", size);
		static const int bufsize = 16384*32;
		static thread_local char lbuf[bufsize];
		auto toread = (bufsize<(size*nmemb))?(bufsize/size):nmemb;
		//printf("argo-buf -> freading up to %ld (sz: %ld)\n", toread, toread*size);
		auto r = f(&lbuf, size, toread, stream);
		//printf("argo-buf -> freading %ld\n", r);
		is_argo_code = was;
		//if(r < 0u) {
		//	printf("argo-buf -> fshit: %ld\n", r);
		//	is_argo_code = was;
		//	return r;
		//}
		std::copy(lbuf, lbuf+(r*size), static_cast<char*>(ptr));
		//printf("returning from fread %ld\n", r);
		fflush(stdout);
		return r;

	}
	//ssize_t read(int fd, void *buf, size_t count) {
	//	printf("calling read\n");
	//	static real_fn<ssize_t, int, void*, size_t> f("read", argo_launcher_read);
	//	bool was = is_argo_code;
	//	auto retval = f(fd, buf, count);
	//	return retval;
	//}
	int lstat(const char *pathname, struct stat *buf) {
		printf("calling lstat\n");
		static real_fn<int, const char*, struct stat*> f("lstat", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(pathname, buf);
		is_argo_code = was;
		return retval;
	}

	int stat(const char *pathname, struct stat *buf) {
		printf("calling stat\n");
		static real_fn<int, const char*, struct stat*> f("stat", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(pathname, buf);
		is_argo_code = was;
		return retval;
	}
	int fstat(int fd, struct stat *buf) {
		printf("calling fstat\n");
		static real_fn<int, int, struct stat*> f("fstat", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(fd, buf);
		is_argo_code = was;
		return retval;
	}
	ssize_t write(int fd, const void *buf, size_t count) {
		//if(is_initialized2)
		//printf("write: buf is argo %d\n", default_global_mempool->is_inside((char*)buf));
		static real_fn<int, int, const void*, size_t> f("write", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		if(is_capturing || !is_initialized2 || !default_global_mempool->is_inside(const_cast<void*>(buf))) {
			//printf("not argo-buf -> calling default fread\n");
			auto r = f(fd, buf, count);
			is_argo_code = was;
			return r;
		}
		static const int bufsize = 16384;
		static thread_local char lbuf[bufsize];
		auto towrite = (bufsize<count)?bufsize:count;
		auto rbuf = static_cast<char*>(const_cast<void*>(buf));
		std::copy(rbuf, rbuf+towrite, lbuf);
		auto r = f(fd, lbuf, towrite);
		//printf("argo-buf -> reading %ld\n", r);
		is_argo_code = was;
		if(r < 0) {
			is_argo_code = was;
			return r;
		}
		return r;
	}
	size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
		//if(is_initialized2)
		//printf("fwrite: buf is argo %d\n", default_global_mempool->is_inside((char*)ptr));
		static real_fn<size_t, const void*, size_t, size_t, FILE*> f("fwrite", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		if(is_capturing || !is_initialized2 || !default_global_mempool->is_inside(const_cast<void*>(ptr))) {
			//printf("not argo-buf -> calling default fread\n");
			auto r = f(ptr, size, nmemb, stream);
			is_argo_code = was;
			return r;
		}
		//printf("argo-buf -> fprocessing sz %ld\n", size);
		static const int bufsize = 16384;
		static thread_local char lbuf[bufsize];
		auto towrite = (bufsize<(size*nmemb))?(bufsize/size):nmemb;
		auto rbuf = static_cast<char*>(const_cast<void*>(ptr));
		std::copy(rbuf, rbuf+towrite, lbuf);
		//printf("argo-buf -> freading up to %ld (sz: %ld)\n", toread, toread*size);
		auto r = f(&lbuf, size, towrite, stream);
		//printf("argo-buf -> freading %ld\n", r);
		is_argo_code = was;
		return r;
	}
	ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
		//printf("calling readlink, %p\n", buf);
		//if(is_initialized2)
		//printf("readlink: buf is argo %d\n", default_global_mempool->is_inside((char*)buf));
		static real_fn<ssize_t, const char*, char*, size_t> f("readlink", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(path, buf, bufsiz);
		is_argo_code = was;
		return retval;
	}
	int close(int fd) {
		//printf("calling close %d\n", fd);
		static real_fn<int, int> f("close", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(fd);
		is_argo_code = was;
		return retval;
	}
	FILE* fopen(const char *path, const char *mode) {
		//printf("calling fopen %s\n", path);
		static real_fn<FILE*,const char*, const char*> f("fopen", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(path, mode);
		//printf("fopen addr: %p\n", retval);
		is_argo_code = was;
		return retval;
	}
	int fclose(FILE* stream) {
		//if(is_initialized2)
		//printf("calling fclose %p\n", stream);
		static real_fn<int, FILE*> f("fclose", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(stream);
		is_argo_code = was;
		return retval;
	}
	int sem_init(sem_t *sem, int pshared, unsigned int value) {
		//printf("calling sem_init %p %d, %u\n", sem, pshared, value);
		static real_fn<int, sem_t*, int, unsigned int> f("sem_init", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		auto retval = f(sem, pshared, value);
		is_argo_code = was;
		return retval;
	}

	int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
		printf("calling instrumented sigaction! signal: %d -> action: %p, oldaction: %p\n", signum, act, oldact);
		static real_fn<int, int, const struct sigaction*, struct sigaction*> f("sigaction", nullptr);
		bool was = is_argo_code;
		is_argo_code = true;
		if(signum != SIGSEGV || is_capturing || !is_initialized2) {
			printf("bypassing instrumentation\n");
			auto retval = f(signum, act, oldact);
			is_argo_code = was;
			return retval;
		}
		if(oldact) {
			//*oldact = old_struct_sigaction;//segfault_sigaction2;
			*oldact = segfault_sigaction2;
		}
		if(act) {
			segfault_sigaction2 = *act;
			//old_struct_sigaction = *act;
		}
		is_argo_code = was;
		return 0;
	}
}
