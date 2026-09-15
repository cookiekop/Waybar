#pragma once

#include <giomm/icon.h>

#include <string>

namespace waybar::util {

Glib::RefPtr<Gio::Icon> resolveAppIcon(const std::string& app_id, const std::string& title);

}  // namespace waybar::util
