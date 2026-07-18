#include "enhanced_rotation/logic.h"

#include <cmath>

namespace enhanced_rotation {

double normalize_rotation(double value) {
    double result = std::fmod(value, 360.0);
    if (result < 0.0) {
        result += 360.0;
    }
    return result;
}

} // namespace enhanced_rotation
