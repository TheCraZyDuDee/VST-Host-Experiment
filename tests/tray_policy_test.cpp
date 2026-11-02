#include "gui/tray_policy.hpp"

#include <cassert>

int main() {
    using vst_host::gui::should_start_hidden_at_login;
    using vst_host::gui::should_hide_close_to_tray;

    for (int flags = 0; flags != 16; ++flags) {
        const bool login = (flags & 1) != 0;
        const bool hide = (flags & 2) != 0;
        const bool enabled = (flags & 4) != 0;
        const bool available = (flags & 8) != 0;
        assert(should_start_hidden_at_login(login, hide, enabled, available) ==
               (login && hide && enabled && available));
    }
    assert(!should_start_hidden_at_login(false, true, true, true));
    assert(!should_start_hidden_at_login(true, true, true, false));
    assert(!should_hide_close_to_tray(true, false, false));
    assert(!should_hide_close_to_tray(true, true, true));
    assert(should_hide_close_to_tray(true, true, false));
    return 0;
}
