#include "SelfTest.h"
#include <iostream>
int main(){try{std::cout<<Serial::SelfTest()<<" offline CDC and byte-routing checks passed.\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
