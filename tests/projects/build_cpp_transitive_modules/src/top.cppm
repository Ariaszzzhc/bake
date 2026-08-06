export module transitive.top;

import transitive.middle;

export int transitive_answer() {
    return Middle{}.leaf.value;
}
