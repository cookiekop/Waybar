#pragma once

#include <giomm/icon.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <json/value.h>

#include <string>
#include <vector>

namespace waybar::modules::niri {

class Workspaces;

class Workspace {
 public:
  Workspace(const Json::Value& workspace_data, Workspaces& manager);
  ~Workspace() = default;

  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;

  Gtk::Widget& widget() { return container_; }
  uint64_t id() const { return id_; }

  void update(const Json::Value& workspace_data, const std::vector<Json::Value>& all_windows,
              const std::string& windows_str, std::size_t total);

 private:
  struct TaskbarWindow {
    uint64_t id;
    std::string app_id;

    bool operator==(const TaskbarWindow&) const = default;
  };

  void updateTaskbar(const std::vector<Json::Value>& my_windows);

  Glib::RefPtr<Gio::Icon> resolveIcon(const std::string& app_id, const std::string& title);

  Workspaces& manager_;
  uint64_t id_;

  // Layout:  container_
  //            ├─ button_       workspace label / icon
  //            └─ taskbar_box_  app icon buttons (shown only when taskbar enabled)
  Gtk::Box container_;
  Gtk::Button button_;
  Gtk::Label label_;
  Gtk::Box taskbar_box_;
  std::vector<TaskbarWindow> taskbar_windows_;
};

}  // namespace waybar::modules::niri
