#include "nova/guest/utils.h"
#include <iostream>
#include <string>

using namespace std;

// This is just a test program to print out the current ip v4 address.
int main(int argc, const char* argv[]) {
    string value = nova::guest::utils::get_ipv4_address("eth0");
    cout << "Ip V4 Address = " << value << endl;
}