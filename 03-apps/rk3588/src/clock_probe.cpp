// Wi-Fi management-side clock calibration. Uses the V4L2 MONOTONIC clock domain.
#include <ctime>
#include <iostream>
#include <string>
#include <cstdint>
int main() {
    std::string request;
    while(std::getline(std::cin,request)) {
        if(request!="clock") return 2;
        timespec t{};
        if(clock_gettime(CLOCK_MONOTONIC,&t)) return 1;
        std::cout << uint64_t(t.tv_sec)*1000000000+uint64_t(t.tv_nsec) << std::endl;
    }
    return 0;
}
