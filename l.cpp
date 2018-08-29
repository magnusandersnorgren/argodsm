#include <unistd.h>
#include <dlfcn.h>

int main() {
	char *argv[] = { "/sw/comp/python/2.7.6_tintin_new/bin/python","-v","test.py", NULL };
    char *envp[] =
    {
        "LD_PRELOAD=../build/lib/liblauncher.so",
				"LD_LIBRARY_PATH=/sw/comp/gcc/6.1.0_tintin/lib64/:/sw/comp/python/2.7.6_tintin_new/lib/",
     NULL
    };
	//dlopen("./build-singlenode/lib/liblauncher.so", RTLD_GLOBAL|RTLD_DEEPBIND);
	execve(argv[0], &argv[0], envp);
}
