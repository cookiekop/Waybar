#include "modules/niri/workspace.hpp"

#include <giomm/appinfo.h>
#include <giomm/desktopappinfo.h>
#include <giomm/icon.h>
#include <giomm/themedicon.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/image.h>
#include <spdlog/spdlog.h>

#include "modules/niri/backend.hpp"
#include "modules/niri/workspaces.hpp"

namespace waybar::modules::niri {
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
      if (auto icon = app_info->get_icon()) {
        spdlog::debug("Niri: matched web app {} to desktop entry {}", app_id, app_info->get_id());
        return icon;
      }
    }
  }

  return {};
}

}  // namespace

Workspace::Workspace(const Json::Value& workspace_data, Workspaces& manager)
    : manager_(manager),
      id_(workspace_data["id"].asUInt64()),
      container_(Gtk::ORIENTATION_HORIZONTAL, 0),
      taskbar_box_(Gtk::ORIENTATION_HORIZONTAL, 0) {
  button_.add(label_);
  container_.pack_start(button_, false, false, 0);
  container_.pack_start(taskbar_box_, false, false, 0);

  container_.set_valign(Gtk::ALIGN_CENTER);
  button_.set_valign(Gtk::ALIGN_CENTER);
  taskbar_box_.set_valign(Gtk::ALIGN_CENTER);
  button_.set_relief(Gtk::RELIEF_NONE);
  container_.get_style_context()->add_class("niri-workspace");

  if (!manager_.config()["disable-click"].asBool()) {
    const auto ws_id = id_;
    button_.signal_pressed().connect([ws_id] {
      try {
        Json::Value request(Json::objectValue);
        auto& action = (request["Action"] = Json::Value(Json::objectValue));
        auto& focusWorkspace = (action["FocusWorkspace"] = Json::Value(Json::objectValue));
        auto& reference = (focusWorkspace["reference"] = Json::Value(Json::objectValue));
        reference["Id"] = ws_id;
        IPC::send(request);
      } catch (const std::exception& e) {
        spdlog::error("Niri: error focusing workspace: {}", e.what());
      }
    });
  }

  container_.show_all();
}

void Workspace::update(const Json::Value& data, const std::vector<Json::Value>& all_windows,
                       const std::string& windows_str, std::size_t total) {
  // ── CSS classes ──────────────────────────────────────────────────────────
  auto style = container_.get_style_context();

  auto setClass = [&](const char* cls, bool on) {
    if (on)
      style->add_class(cls);
    else
      style->remove_class(cls);
  };

  setClass("focused", data["is_focused"].asBool());
  setClass("active", data["is_active"].asBool());
  setClass("urgent", data["is_urgent"].asBool());
  setClass("empty", data["active_window_id"].isNull());
  setClass("current_output",
           data["output"] && data["output"].asString() == manager_.bar().output->name);

  // ── Workspace label ───────────────────────────────────────────────────────
  std::string name;
  if (data["name"]) {
    name = data["name"].asString();
  } else {
    name = std::to_string(data["idx"].asUInt());
  }

  container_.set_name("niri-workspace-" + name);

  const auto& cfg = manager_.config();

  if (cfg["format"].isString()) {
    auto format = cfg["format"].asString();
    name = fmt::format(fmt::runtime(format), fmt::arg("icon", manager_.getIcon(name, data)),
                       fmt::arg("value", name), fmt::arg("name", data["name"].asString()),
                       fmt::arg("index", data["idx"].asUInt()),
                       fmt::arg("output", data["output"].asString()), fmt::arg("total", total),
                       fmt::arg("windows", windows_str));
  }

  if (!cfg["disable-markup"].asBool()) {
    label_.set_markup(name);
  } else {
    label_.set_text(name);
  }

  // ── Visibility ───────────────────────────────────────────────────────────
  const bool alloutputs = cfg["all-outputs"].asBool();
  if (cfg["current-only"].asBool()) {
    const auto* prop = alloutputs ? "is_focused" : "is_active";
    data[prop].asBool() ? container_.show() : container_.hide();
  } else if (cfg["hide-empty"].asBool()) {
    (data["active_window_id"].isNull() && !data["is_focused"].asBool()) ? container_.hide()
                                                                        : container_.show();
  } else {
    container_.show();
  }

  // ── Taskbar ───────────────────────────────────────────────────────────────
  const auto& taskbar_cfg = cfg["workspace-taskbar"];
  if (taskbar_cfg.isObject() && taskbar_cfg["enable"].asBool()) {
    std::vector<Json::Value> my_windows;
    for (const auto& win : all_windows) {
      if (win["workspace_id"].asUInt64() == id_) {
        my_windows.push_back(win);
      }
    }

    std::sort(my_windows.begin(), my_windows.end(), [](const Json::Value& a, const Json::Value& b) {
      const auto& la = a["layout"];
      const auto& lb = b["layout"];
      const bool ha = la.isObject() && la["pos_in_scrolling_layout"].isArray();
      const bool hb = lb.isObject() && lb["pos_in_scrolling_layout"].isArray();
      if (!ha && !hb) return false;
      if (!ha) return false;
      if (!hb) return true;
      const int col_a = la["pos_in_scrolling_layout"][0].asInt();
      const int col_b = lb["pos_in_scrolling_layout"][0].asInt();
      if (col_a != col_b) return col_a < col_b;
      return la["pos_in_scrolling_layout"][1].asInt() < lb["pos_in_scrolling_layout"][1].asInt();
    });

    updateTaskbar(my_windows);
    taskbar_box_.show();
    button_.hide();
  } else {
    for (auto* child : taskbar_box_.get_children()) {
      taskbar_box_.remove(*child);
    }
    taskbar_box_.hide();
    button_.show();
    taskbar_windows_.clear();
  }
}

// ── Taskbar update ───────────────────────────────────────────────────────────

void Workspace::updateTaskbar(const std::vector<Json::Value>& my_windows) {
  std::vector<TaskbarWindow> next_windows;
  next_windows.reserve(my_windows.size());
  for (const auto& win : my_windows) {
    next_windows.push_back({win["id"].asUInt64(), win["app_id"].asString()});
  }

  const auto children = taskbar_box_.get_children();
  if (next_windows == taskbar_windows_ && children.size() == my_windows.size()) {
    for (std::size_t i = 0; i < my_windows.size(); ++i) {
      auto* btn = dynamic_cast<Gtk::Button*>(children[i]);
      if (btn == nullptr) return;

      auto style = btn->get_style_context();
      if (my_windows[i]["is_focused"].asBool()) {
        style->add_class("focused");
      } else {
        style->remove_class("focused");
      }
      btn->set_tooltip_text(my_windows[i]["title"].asString());
    }
    return;
  }

  for (auto* child : taskbar_box_.get_children()) {
    taskbar_box_.remove(*child);
  }
  taskbar_windows_ = std::move(next_windows);

  const auto& taskbar_cfg = manager_.config()["workspace-taskbar"];
  const int icon_size = taskbar_cfg["icon-size"].isInt() ? taskbar_cfg["icon-size"].asInt() : 16;

  for (const auto& win : my_windows) {
    const auto win_id = win["id"].asUInt64();
    const std::string app_id = win["app_id"].isString() ? win["app_id"].asString() : "";
    const std::string title = win["title"].isString() ? win["title"].asString() : app_id;
    const bool is_focused = win["is_focused"].asBool();

    auto* btn = Gtk::make_managed<Gtk::Button>();
    btn->set_valign(Gtk::ALIGN_CENTER);
    btn->set_relief(Gtk::RELIEF_NONE);
    btn->set_focus_on_click(false);
    btn->get_style_context()->add_class("niri-taskbar-btn");
    if (is_focused) btn->get_style_context()->add_class("focused");
    btn->set_tooltip_text(title);

    auto icon = resolveIcon(app_id, title);
    if (icon) {
      auto* img = Gtk::make_managed<Gtk::Image>(icon, Gtk::ICON_SIZE_INVALID);
      img->set_pixel_size(icon_size);
      img->set_size_request(icon_size, icon_size);
      img->set_halign(Gtk::ALIGN_CENTER);
      img->set_valign(Gtk::ALIGN_CENTER);
      btn->add(*img);
    } else {
      std::string fallback = app_id.empty() ? title : app_id;
      if (!fallback.empty()) {
        fallback = fallback.substr(0, 3);
      } else {
        fallback = "?";
      }
      auto* lbl = Gtk::make_managed<Gtk::Label>(fallback);
      lbl->set_valign(Gtk::ALIGN_CENTER);
      btn->add(*lbl);
    }

    btn->signal_pressed().connect([win_id] {
      try {
        Json::Value request(Json::objectValue);
        auto& action = (request["Action"] = Json::Value(Json::objectValue));
        auto& focus_window = (action["FocusWindow"] = Json::Value(Json::objectValue));
        focus_window["id"] = win_id;
        IPC::send(request);
      } catch (const std::exception& e) {
        spdlog::error("Niri: error focusing window {}: {}", win_id, e.what());
      }
    });

    btn->signal_button_press_event().connect([win_id](GdkEventButton* event) -> bool {
      if (event->button != GDK_BUTTON_MIDDLE) return false;

      try {
        Json::Value request(Json::objectValue);
        auto& action = (request["Action"] = Json::Value(Json::objectValue));
        auto& close_window = (action["CloseWindow"] = Json::Value(Json::objectValue));
        close_window["id"] = win_id;
        IPC::send(request);
      } catch (const std::exception& e) {
        spdlog::error("Niri: error closing window {}: {}", win_id, e.what());
      }
      return true;
    });

    taskbar_box_.pack_start(*btn, false, false, 0);
    btn->show_all();
  }
}

// ── Icon loading ─────────────────────────────────────────────────────────────

Glib::RefPtr<Gio::Icon> Workspace::resolveIcon(const std::string& app_id,
                                               const std::string& title) {
  if (app_id.empty()) return {};

  if (auto app_info = Gio::DesktopAppInfo::create(app_id + ".desktop")) {
    if (auto icon = app_info->get_icon()) return icon;
  }

  if (auto icon = resolveWebAppIcon(app_id, title)) return icon;

  auto theme = Gtk::IconTheme::get_default();
  auto try_icon = [&theme](const std::string& name) -> Glib::RefPtr<Gio::Icon> {
    if (!theme->has_icon(name)) return {};
    return Gio::ThemedIcon::create(name);
  };

  if (auto icon = try_icon(app_id)) return icon;

  std::string lower = app_id;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  if (auto icon = try_icon(lower)) return icon;

  const auto dot = app_id.rfind('.');
  if (dot != std::string::npos) {
    std::string last = app_id.substr(dot + 1);
    std::transform(last.begin(), last.end(), last.begin(), ::tolower);
    if (auto icon = try_icon(last)) return icon;
  }

  return {};
}

}  // namespace waybar::modules::niri
