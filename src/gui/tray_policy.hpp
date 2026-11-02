#pragma once

namespace vst_host::gui {

// A hidden login launch is safe only when an actual tray is available.
// If no StatusNotifierItem/XEmbed tray is offered by the desktop, always
// show the main window so the user cannot lose access to the application.
[[nodiscard]] constexpr bool should_start_hidden_at_login(
    bool launched_at_login, bool requested_hidden,
    bool tray_enabled, bool tray_available) noexcept {
    return launched_at_login && requested_hidden &&
           tray_enabled && tray_available;
}

[[nodiscard]] constexpr bool should_hide_close_to_tray(
    bool tray_enabled, bool tray_available, bool explicit_quit) noexcept {
    return tray_enabled && tray_available && !explicit_quit;
}

} // namespace vst_host::gui
