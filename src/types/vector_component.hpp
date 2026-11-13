#pragma once
#include <cmath>
#include <limits>
#include <stdexcept>
namespace minivectordb::detail {
inline float vector_component(double value) {
    // IEEE binary32 round-to-nearest overflow begins half an ULP above
    // FLT_MAX. Decimal literals such as 3.4028235e38 still round to FLT_MAX.
    // Handle that interval explicitly rather than casting an out-of-range double.
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559 &&
                  std::numeric_limits<float>::digits==24);
    const double maximum=std::numeric_limits<float>::max();
    const double overflow=std::ldexp(1.0,128)-std::ldexp(1.0,103);
    const double magnitude=std::abs(value);
    if(!std::isfinite(value) || magnitude>=overflow) throw std::invalid_argument("VECTOR component is not finite float32");
    const float converted=magnitude>maximum
        ? (std::signbit(value)?-std::numeric_limits<float>::max():std::numeric_limits<float>::max())
        : static_cast<float>(value);
    if(!std::isfinite(converted)) throw std::invalid_argument("VECTOR component is not finite float32");
    return converted;
}
}
