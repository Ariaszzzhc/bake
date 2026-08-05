#include <iostream>
#include "cmathlib.h"

int main() {
    int sum = cmathlib_add(2, 3);
    int diff = cmathlib_sub(10, 4);
    std::cout << "add(2,3) = " << sum << "\n";
    std::cout << "sub(10,4) = " << diff << "\n";
    // Return 0 if values are correct
    return (sum == 5 && diff == 6) ? 0 : 1;
}
