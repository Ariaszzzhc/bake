#include <iostream>
#include "cmathlib.h"

int main() {
    int sum = cmathlib_add(7, 8);
    int diff = cmathlib_sub(20, 5);
    std::cout << "add(7,8) = " << sum << "\n";
    std::cout << "sub(20,5) = " << diff << "\n";
    return (sum == 15 && diff == 15) ? 0 : 1;
}
