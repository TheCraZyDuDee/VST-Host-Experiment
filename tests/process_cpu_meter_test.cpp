#include "gui/process_cpu_meter.hpp"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using vst_host::gui::process_cpu_percent;

    assert(process_cpu_percent(0.25, 1.0) == 25.0);
    assert(process_cpu_percent(0.50, 0.25) == 200.0);
    assert(process_cpu_percent(0.0, 0.1) == 0.0);
    assert(process_cpu_percent(-1.0, 1.0) == 0.0);
    assert(process_cpu_percent(1.0, -1.0) == 0.0);
    assert(process_cpu_percent(1.0, 0.0) == 0.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    assert(process_cpu_percent(nan, 1.0) == 0.0);
    assert(process_cpu_percent(1.0, nan) == 0.0);
    assert(process_cpu_percent(inf, 1.0) == 0.0);

    // Real clock sampling may be close to zero when the process is idle,
    // but must always return a finite nonnegative number and reset cleanly.
    vst_host::gui::ProcessCpuMeter meter;
    assert(meter.sample() == 0.0);
    assert(std::isfinite(meter.sample()));
    meter.reset();
    assert(meter.sample() == 0.0);
    return 0;
}
