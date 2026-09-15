#include "util/app_icon.hpp"

#include <giomm/appinfo.h>
#include <giomm/desktopappinfo.h>
#include <giomm/themedicon.h>
#include <gtkmm/icontheme.h>

#include <algorithm>
#include <cctype>

namespace waybar::util {
namespace {

std::string normalizeDesktopField(const std::string& value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (const unsigned char character : value) {
    if (std::isalnum(character)) normalized.push_back(std::tolower(character));
  }
  return normalized;
}

Glib::RefPtr<Gio::Icon> resolveWebAppIcon(const std::string& app_id, const std::string& title) {
  if (!app_id.starts_with("chrome-")) return {};

  std::string app_key = normalizeDesktopField(app_id);
  app_key.erase(0, std::string("chrome").size());
  const auto title_key = normalizeDesktopField(title);

  for (const auto& app_info : Gio::AppInfo::get_all()) {
    const auto commandline = app_info->get_commandline();
    if (commandline.find("watchtower-launch-webapp") == std::string::npos &&
        commandline.find("--app=") == std::string::npos &&
        commandline.find("--app-id=") == std::string::npos) {
      continue;
    }

    const auto desktop_info = Glib::RefPtr<Gio::DesktopAppInfo>::cast_dynamic(app_info);
    std::string startup_class;
    if (desktop_info) startup_class = normalizeDesktopField(desktop_info->get_startup_wm_class());
    if (startup_class.starts_with("crx")) startup_class.erase(0, 3);

    const auto commandline_key = normalizeDesktopField(commandline);
    const auto desktop_id_key = normalizeDesktopField(app_info->get_id());
    const auto display_name_key = normalizeDesktopField(app_info->get_display_name());
    const bool id_matches =
        !app_key.empty() && (commandline_key.find(app_key) != std::string::npos ||
                            desktop_id_key.find(app_key) != std::string::npos);
    const bool startup_class_matches =
        !startup_class.empty() && app_key.find(startup_class) != std::string::npos;
    const bool title_matches =
        display_name_key.size() >= 3 && title_key.find(display_name_key) != std::string::npos;

    if (id_matches || startup_class_matches || title_matches) {
      if (auto icon = app_info->get_icon()) return icon;
    }
  }

  return {};
}

}  // namespace

Glib::RefPtr<Gio::Icon> resolveAppIcon(const std::string& app_id, const std::string& title) {
  if (app_id.empty()) return {};
  if (auto app_info = Gio::DesktopAppInfo::create(app_id + ".desktop")) {
    if (auto icon = app_info->get_icon()) return icon;
  }
  if (auto icon = resolveWebAppIcon(app_id, title)) return icon;

  auto theme = Gtk::IconTheme::get_default();
  const auto try_icon = [&theme](const std::string& name) -> Glib::RefPtr<Gio::Icon> {
    if (!theme->has_icon(name)) return {};
    return Gio::ThemedIcon::create(name);
  };
  if (auto icon = try_icon(app_id)) return icon;

  const auto lower_case = [](unsigned char character) { return std::tolower(character); };
  std::string lower = app_id;
  std::transform(lower.begin(), lower.end(), lower.begin(), lower_case);
  if (auto icon = try_icon(lower)) return icon;
  const auto dot = app_id.rfind('.');
  if (dot != std::string::npos) {
    std::string last = app_id.substr(dot + 1);
    std::transform(last.begin(), last.end(), last.begin(), lower_case);
    if (auto icon = try_icon(last)) return icon;
  }
  return {};
}

}  // namespace waybar::util
