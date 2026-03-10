#pragma once

namespace cdr {

enum class Error {
    ConractWithoutNPV = 0,
    __NumberOfErrors,
};

inline cdr::Failure<Error> ErrorContractWithoutNPV() {
    return cdr::Failure<Error>(Error::ConractWithoutNPV);
}

} // namespace cdr