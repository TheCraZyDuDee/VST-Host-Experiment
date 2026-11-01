#include "audio/nonfinite_sanitizer.hpp"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using vst_host::audio::sanitize_nonfinite_stereo;

    float valid[] = {0.0F, -0.0F, 0.25F, -0.25F,
                     1.5F, -2.0F, 0.0F, 1.0F};
    assert(sanitize_nonfinite_stereo(valid, 4) == 0);
    assert(valid[2] == 0.25F && valid[3] == -0.25F);
    assert(valid[4] == 1.5F && valid[5] == -2.0F);

    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();
    float invalid[] = {nan, inf, -inf, 0.5F, -0.5F, nan, inf, -inf};
    assert(sanitize_nonfinite_stereo(invalid, 4) == 6);
    assert(invalid[0] == 0.0F && invalid[1] == 0.0F);
    assert(invalid[2] == 0.0F && invalid[3] == 0.5F);
    assert(invalid[4] == -0.5F && invalid[5] == 0.0F);
    assert(invalid[6] == 0.0F && invalid[7] == 0.0F);
    assert(sanitize_nonfinite_stereo(invalid, 4) == 0);
    for (const auto sample : invalid) assert(std::isfinite(sample));

    assert(sanitize_nonfinite_stereo(nullptr, 20) == 0);
    assert(sanitize_nonfinite_stereo(valid, 0) == 0);
    return 0;
}
