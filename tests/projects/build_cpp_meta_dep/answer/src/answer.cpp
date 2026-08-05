#include <answer/answer.hpp>
#include <base/base.hpp>

#ifndef ANSWER_BIAS
#define ANSWER_BIAS 0
#endif

int answer() {
    return base_answer() + ANSWER_BIAS;
}
