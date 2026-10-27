#include "util/app_icon.hpp"

#include <giomm/appinfo.h>
#include <giomm/desktopappinfo.h>
#include <giomm/themedicon.h>
#include <glibmm/shell.h>
#include <gtkmm/icontheme.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>

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

  const auto title_key = normalizeDesktopField(title);
  Glib::RefPtr<Gio::Icon> title_icon;

  for (const auto& app_info : Gio::AppInfo::get_all()) {
    const auto commandline = app_info->get_commandline();
    if (commandline.find("watchtower-launch-webapp") == std::string::npos &&
        commandline.find("--app=") == std::string::npos &&
        commandline.find("--app-id=") == std::string::npos) {
      continue;
    }

    const auto desktop_info = Glib::RefPtr<Gio::DesktopAppInfo>::cast_dynamic(app_info);
    std::string startup_class;
    if (desktop_info) startup_class = desktop_info->get_startup_wm_class();
    if (matchesWebApp(app_id, commandline, startup_class)) {
      if (auto icon = app_info->get_icon()) return icon;
    }
    const auto display_name_key = normalizeDesktopField(app_info->get_display_name());
    const bool title_matches =
        display_name_key.size() >= 3 && title_key.find(display_name_key) != std::string::npos;
    if (!title_icon && title_matches) title_icon = app_info->get_icon();
  }

  return title_icon;
}

}  // namespace

bool matchesWebApp(const std::string& app_id, const std::string& commandline,
                   const std::string& startup_class) {
  if (!app_id.starts_with("chrome-")) return false;
  if (!startup_class.empty() && app_id == startup_class) return true;

  std::string identity;
  std::string profile;
  try {
    const std::vector<std::string> arguments = Glib::shell_parse_argv(commandline);
    for (size_t i = 0; i < arguments.size(); ++i) {
      const auto& argument = arguments[i];
      if (argument.starts_with("--app-id=")) {
        identity = argument.substr(9);
      } else if (argument.starts_with("--app=")) {
        std::unique_ptr<GUri, decltype(&g_uri_unref)> uri(
            g_uri_parse(argument.substr(6).c_str(), G_URI_FLAGS_NONE, nullptr), g_uri_unref);
        if (!uri || !g_uri_get_host(uri.get())) return false;
        identity = g_uri_get_host(uri.get());
        identity += '_';
        const auto* path = g_uri_get_path(uri.get());
        identity += path && *path ? path : "/";
        std::replace(identity.begin(), identity.end(), '/', '_');
      } else if (argument.starts_with("--profile-directory=")) {
        profile = argument.substr(20);
      } else if (argument == "--profile-directory" && i + 1 < arguments.size()) {
        profile = arguments[++i];
      }
    }
  } catch (const Glib::ShellError&) {
    return false;
  }
  if (identity.empty()) {
    if (startup_class.starts_with("crx_")) identity = startup_class.substr(4);
    if (identity.empty()) return false;
  }
  const auto prefix = "chrome-" + identity + '-';
  if (!app_id.starts_with(prefix)) return false;
  if (profile.empty()) return true;
  std::replace(profile.begin(), profile.end(), ' ', '_');
  return app_id == prefix + profile;
}

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
