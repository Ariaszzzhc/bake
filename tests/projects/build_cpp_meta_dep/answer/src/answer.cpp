#include <answer/answer.hpp>
#include <base/base.hpp>

#ifndef BAKE_ANSWER_BIASED
#error "dependency option macro was not generated"
#endif

int answer() {
    return base_answer() + BAKE_ANSWER_BIASED;
}
