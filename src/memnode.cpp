#include <iostream>
#include <mpi.h>
#include "argo.hpp"
int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	std::cout << "starting an ArgoDSM memory-only node" << std::endl;
	argo::init(10l*1024l*1024l*1024l);
	std::cout << "ArgoDSM memory-only node " << argo::backend::node_id() << " out of " <<  argo::backend::number_of_nodes() << " ArgoDSM nodes is ready" << std::endl;
	//argo::data_distribution::global_ptr<bool> argo_alive(argo::conew_<bool>(true));
	//while(argo::backend::atomic::load(argo_alive)) {
	//	int flag;
	//	MPI_Iprobe(MPI_ANY_SOURCE,MPI_ANY_TAG,MPI_COMM_WORLD,&flag,MPI_STATUS_IGNORE);
	//}
	//argo::barrier();
	argo::finalize();
	std::cout << "ArgoDSM memory-only node " << argo::backend::node_id() << " terminated" << std::endl;
	return 0;
}
