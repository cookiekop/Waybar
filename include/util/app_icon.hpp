#pragma once

#include <giomm/icon.h>

#include <string>

namespace waybar::util {

bool matchesWebApp(const std::string& app_id, const std::string& commandline,
                   const std::string& startup_class = "");
Glib::RefPtr<Gio::Icon> resolveAppIcon(const std::string& app_id, const std::string& title);

}  // namespace waybar::util
