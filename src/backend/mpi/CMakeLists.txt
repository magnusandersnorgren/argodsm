# Copyright (C) Eta Scale AB. Licensed under the Eta Scale Open Source License. See the LICENSE file for details.


add_library(argobackend-mpi mpi.cpp swdsm.cpp prefetcher.cpp data_transfer.cpp)
link_directories("/home/mano9519/projfolder/starpu-1.1.7/build/lib")
target_link_libraries(argobackend-mpi rt "/home/mano9519/projfolder/starpu-1.1.7/build/lib/libstarpu-1.1.so" "/home/mano9519/projfolder/fxt/build/lib/libfxt.so")
target_link_libraries(argobackend-mpi rt)

install(TARGETS argobackend-mpi
	COMPONENT "Runtime"
	RUNTIME DESTINATION bin
	LIBRARY DESTINATION lib
	ARCHIVE DESTINATION lib)
