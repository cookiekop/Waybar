#include "modules/mango/workspaces.hpp"

#include <giomm/appinfo.h>
#include <giomm/desktopappinfo.h>
#include <giomm/themedicon.h>
#include <gtkmm/icontheme.h>
#include <gtkmm/image.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace waybar::modules::mango {
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

Workspaces::TagWidget::TagWidget(uint64_t idx)
    : index(idx),
      container(Gtk::ORIENTATION_HORIZONTAL, 0),
      taskbar(Gtk::ORIENTATION_HORIZONTAL, 0) {
  button.add(label);
  container.pack_start(button, false, false, 0);
  container.pack_start(taskbar, false, false, 0);
  container.set_valign(Gtk::ALIGN_CENTER);
  button.set_valign(Gtk::ALIGN_CENTER);
  taskbar.set_valign(Gtk::ALIGN_CENTER);
  button.set_relief(Gtk::RELIEF_NONE);
  container.get_style_context()->add_class("mango-workspace");
}

Workspaces::Workspaces(const std::string& id, const Bar& bar, const Json::Value& config)
    : AModule(config, "workspaces", id, false, false), bar_(bar), box_(bar.orientation, 0) {
  box_.set_name("workspaces");
  if (!id.empty()) box_.get_style_context()->add_class(id);
  box_.get_style_context()->add_class(MODULE_CLASS);
  event_box_.add(box_);

  if (config_["on-click"].isString()) on_click_left_ = config_["on-click"].asString();
  if (config_["on-click-middle"].isString())
    on_click_middle_ = config_["on-click-middle"].asString();
  if (config_["on-click-right"].isString()) on_click_right_ = config_["on-click-right"].asString();

  overview_button_ = new Gtk::Button("OVERVIEW");
  overview_button_->set_relief(Gtk::RELIEF_NONE);
  box_.pack_start(*overview_button_, false, false, 0);

  if (!on_click_left_.empty() || !on_click_middle_.empty() || !on_click_right_.empty()) {
    overview_button_->add_events(Gdk::BUTTON_PRESS_MASK);
    overview_button_->signal_button_press_event().connect(
        [this](GdkEventButton* event) -> bool { return handleButtonClick(event, 0, true); }, false);
  }

  IPC::getInstance().registerForIPC("monitor", this);
  IPC::getInstance().registerForIPC("client", this);
}

Workspaces::~Workspaces() {
  IPC::getInstance().unregisterForIPC(this);

  if (overview_button_) {
    box_.remove(*overview_button_);
    delete overview_button_;
    overview_button_ = nullptr;
  }

  for (auto& [idx, widget] : tags_) box_.remove(widget->container);
  tags_.clear();
}

void Workspaces::onEvent(const Json::Value& /*event*/) { dp.emit(); }

void Workspaces::doUpdate() {
  Json::Value monitor = IPC::getInstance().getMonitor(bar_.output->name);
  if (monitor.isNull()) return;

  const auto& monitor_tags = monitor["tags"];
  const auto clients = IPC::getInstance().getClients();
  bool overview_mode = false;
  if (monitor["active_tags"].isArray()) {
    const auto& active_tags = monitor["active_tags"];
    overview_mode = active_tags.size() == 1 && active_tags[0].asInt() == 0;
  }

  for (auto& [idx, widget] : tags_) widget->container.hide();

  if (overview_mode) {
    overview_button_->show();
    auto style = overview_button_->get_style_context();
    style->add_class("overview");
    if (monitor["active"].asBool())
      style->add_class("current_output");
    else
      style->remove_class("current_output");

    const std::string label =
        config_["overview-label"].isString() ? config_["overview-label"].asString() : "OVERVIEW";
    if (!config_["disable-markup"].asBool()) {
      if (auto gtk_label = dynamic_cast<Gtk::Label*>(overview_button_->get_child())) {
        gtk_label->set_markup(label);
      }
    } else {
      overview_button_->set_label(label);
    }
    return;
  }

  overview_button_->hide();
  for (auto it = tags_.begin(); it != tags_.end();) {
    const uint64_t index = it->first;
    const bool found =
        std::any_of(monitor_tags.begin(), monitor_tags.end(),
                    [index](const Json::Value& tag) { return tag["index"].asUInt64() == index; });
    if (found) {
      ++it;
    } else {
      box_.remove(it->second->container);
      it = tags_.erase(it);
    }
  }

  std::vector<uint64_t> indices;
  for (const auto& tag : monitor_tags) {
    const uint64_t index = tag["index"].asUInt64();
    auto it = tags_.find(index);
    TagWidget& widget = it == tags_.end() ? addTag(index) : *it->second;
    updateTag(widget, tag, monitor, clients);
    indices.push_back(index);
  }

  std::sort(indices.begin(), indices.end());
  for (std::size_t position = 0; position < indices.size(); ++position) {
    box_.reorder_child(tags_[indices[position]]->container, static_cast<int>(position + 1));
  }
}

void Workspaces::update() {
  doUpdate();
  AModule::update();
}

Workspaces::TagWidget& Workspaces::addTag(uint64_t index) {
  auto widget = std::make_unique<TagWidget>(index);
  auto& result = *widget;
  box_.pack_start(result.container, false, false, 0);

  if (!on_click_left_.empty() || !on_click_middle_.empty() || !on_click_right_.empty()) {
    result.button.add_events(Gdk::BUTTON_PRESS_MASK);
    result.button.signal_button_press_event().connect(
        [this, index](GdkEventButton* event) -> bool {
          return handleButtonClick(event, index, false);
        },
        false);
  }

  result.container.show_all();
  tags_.emplace(index, std::move(widget));
  return result;
}

void Workspaces::updateTag(TagWidget& widget, const Json::Value& tag, const Json::Value& monitor,
                           const std::vector<Json::Value>& clients) {
  auto style = widget.container.get_style_context();
  const bool active = tag["is_active"].asBool();
  const bool urgent = tag["is_urgent"].asBool();
  const bool empty = tag["client_count"].asInt() == 0;
  const bool current_output = monitor["active"].asBool();
  const auto set_class = [&style](const char* name, bool enabled) {
    if (enabled)
      style->add_class(name);
    else
      style->remove_class(name);
  };
  set_class("active", active);
  set_class("focused", active && current_output);
  set_class("urgent", urgent);
  set_class("empty", empty);
  set_class("current_output", current_output);

  const uint64_t index = tag["index"].asUInt64();
  std::string name = std::to_string(index);
  widget.container.set_name("mango-workspace-" + name);
  if (config_["format"].isString()) {
    name = fmt::format(fmt::runtime(config_["format"].asString()),
                       fmt::arg("icon", getIcon(name, tag)), fmt::arg("value", name),
                       fmt::arg("name", name), fmt::arg("index", index),
                       fmt::arg("output", monitor["name"].asString()));
  }
  if (config_["disable-markup"].asBool())
    widget.label.set_text(name);
  else
    widget.label.set_markup(name);

  std::vector<Json::Value> tag_clients;
  for (const auto& client : clients) {
    if (client["monitor"].asString() != monitor["name"].asString()) continue;
    const auto& client_tags = client["tags"];
    const bool has_tag =
        client_tags.isArray() &&
        std::any_of(client_tags.begin(), client_tags.end(),
                    [index](const Json::Value& value) { return value.asUInt64() == index; });
    if (has_tag) tag_clients.push_back(client);
  }
  std::sort(tag_clients.begin(), tag_clients.end(), [](const auto& left, const auto& right) {
    if (left["x"].asInt() != right["x"].asInt()) return left["x"].asInt() < right["x"].asInt();
    return left["y"].asInt() < right["y"].asInt();
  });

  const auto& taskbar_config = config_["workspace-taskbar"];
  if (taskbar_config.isObject() && taskbar_config["enable"].asBool() && !tag_clients.empty()) {
    updateTaskbar(widget, tag_clients);
    widget.taskbar.show();
    widget.button.hide();
  } else {
    updateTaskbar(widget, {});
    widget.taskbar.hide();
    widget.button.show();
  }

  if (config_["current-only"].asBool()) {
    active ? widget.container.show() : widget.container.hide();
  } else if (config_["hide-empty"].asBool() && empty && !active) {
    widget.container.hide();
  } else {
    widget.container.show();
  }
}

void Workspaces::updateTaskbar(TagWidget& widget, const std::vector<Json::Value>& clients) {
  std::vector<TaskbarWindow> next_windows;
  next_windows.reserve(clients.size());
  for (const auto& client : clients) {
    next_windows.push_back({client["id"].asUInt64(), client["appid"].asString()});
  }

  const auto children = widget.taskbar.get_children();
  if (next_windows == widget.taskbar_windows && children.size() == clients.size()) {
    for (std::size_t index = 0; index < clients.size(); ++index) {
      auto* button = dynamic_cast<Gtk::Button*>(children[index]);
      if (!button) return;
      auto style = button->get_style_context();
      if (clients[index]["is_focused"].asBool())
        style->add_class("focused");
      else
        style->remove_class("focused");
      button->set_tooltip_text(clients[index]["title"].asString());
    }
    return;
  }

  for (auto* child : children) widget.taskbar.remove(*child);
  widget.taskbar_windows = std::move(next_windows);
  const auto& taskbar_config = config_["workspace-taskbar"];
  const int icon_size =
      taskbar_config["icon-size"].isInt() ? taskbar_config["icon-size"].asInt() : 16;

  for (const auto& client : clients) {
    const uint64_t client_id = client["id"].asUInt64();
    const std::string app_id = client["appid"].asString();
    const std::string title = client["title"].isString() ? client["title"].asString() : app_id;
    auto* button = Gtk::make_managed<Gtk::Button>();
    button->set_valign(Gtk::ALIGN_CENTER);
    button->set_relief(Gtk::RELIEF_NONE);
    button->set_focus_on_click(false);
    button->get_style_context()->add_class("mango-taskbar-btn");
    if (client["is_focused"].asBool()) button->get_style_context()->add_class("focused");
    button->set_tooltip_text(title);

    if (auto icon = resolveIcon(app_id, title)) {
      auto* image = Gtk::make_managed<Gtk::Image>(icon, Gtk::ICON_SIZE_INVALID);
      image->set_pixel_size(icon_size);
      image->set_size_request(icon_size, icon_size);
      image->set_halign(Gtk::ALIGN_CENTER);
      image->set_valign(Gtk::ALIGN_CENTER);
      button->add(*image);
    } else {
      std::string fallback = app_id.empty() ? title : app_id;
      fallback = fallback.empty() ? "?" : fallback.substr(0, 3);
      button->add(*Gtk::make_managed<Gtk::Label>(fallback));
    }

    button->signal_pressed().connect([client_id] {
      Json::Value request;
      request["command"] = "dispatch focusid client," + std::to_string(client_id);
      IPC::sendAsync(request);
    });
    button->signal_button_press_event().connect([client_id](GdkEventButton* event) -> bool {
      if (event->button != GDK_BUTTON_MIDDLE) return false;
      Json::Value request;
      request["command"] = "dispatch killclient client," + std::to_string(client_id);
      IPC::sendAsync(request);
      return true;
    });

    widget.taskbar.pack_start(*button, false, false, 0);
    button->show_all();
  }
}

Glib::RefPtr<Gio::Icon> Workspaces::resolveIcon(const std::string& app_id,
                                                const std::string& title) {
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

std::string Workspaces::getIcon(const std::string& value, const Json::Value& tag) {
  const auto& icons = config_["format-icons"];
  if (!icons) return value;
  if (tag["is_urgent"].asBool() && icons["urgent"]) return icons["urgent"].asString();
  if (tag["is_active"].asBool() && icons["active"]) return icons["active"].asString();
  if (tag["client_count"].asInt() == 0 && icons["empty"]) return icons["empty"].asString();
  const std::string index = std::to_string(tag["index"].asUInt());
  if (icons[index]) return icons[index].asString();
  if (icons["default"]) return icons["default"].asString();
  return value;
}

bool Workspaces::handleButtonClick(GdkEventButton* event, uint64_t index, bool is_overview) {
  std::string action;
  if (event->button == 1)
    action = on_click_left_;
  else if (event->button == 2)
    action = on_click_middle_;
  else if (event->button == 3)
    action = on_click_right_;
  if (action.empty()) return true;

  std::string command;
  if (is_overview) {
    if (action == "activate") command = "dispatch overview";
    if (action == "toggle") command = "dispatch toggleoverview";
  } else {
    if (action == "activate") command = "dispatch view," + std::to_string(index);
    if (action == "toggle") command = "dispatch toggleview," + std::to_string(index);
  }
  if (!command.empty()) {
    if (!is_overview && action == "activate" && config_["synchronize"].asBool()) command += ",1";
    Json::Value request;
    request["command"] = command;
    IPC::sendAsync(request);
  }
  return true;
}

}  // namespace waybar::modules::mango
