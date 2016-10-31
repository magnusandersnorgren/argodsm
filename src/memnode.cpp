#include <iostream>
#include "argo.hpp"
int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	std::cout << "starting an ArgoDSM memory-only node" << std::endl;
	argo::init(4l*1024*1024*1024);
	std::cout << "ArgoDSM memory-only node " << argo::backend::node_id() << " out of " <<  argo::backend::number_of_nodes() << " ArgoDSM nodes is ready" << std::endl;
	argo::finalize();
	std::cout << "ArgoDSM memory-only node " << argo::backend::node_id() << " terminated" << std::endl;
	return 0;
}
