import extension.api;
import extension.detail;

#include <extension/config.hh>

extern "C" int c_extension_value(void);
int cc_extension_value();
int cxx_extension_value();
int objective_cpp_extension_value();

int main() {
    const int headers = EXTENSION_H_VALUE + EXTENSION_HPP_VALUE +
                        EXTENSION_HXX_VALUE + EXTENSION_HH_VALUE;
    const int sources = c_extension_value() + cc_extension_value() +
                        cxx_extension_value() + objective_cpp_extension_value();
    return headers == 10 && sources == 34 &&
                   public_module_value() == 5 &&
                   private_module_value() == 6
        ? 0
        : 1;
}
