#pragma once

#include <giomm/icon.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <json/value.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "AModule.hpp"
#include "bar.hpp"
#include "modules/mango/backend.hpp"

namespace waybar::modules::mango {

class Workspaces : public AModule, public EventHandler {
 public:
  Workspaces(const std::string&, const Bar&, const Json::Value&);
  ~Workspaces() override;
  void update() override;

 private:
  struct TaskbarWindow {
    uint64_t id;
    std::string app_id;

    bool operator==(const TaskbarWindow&) const = default;
  };

  struct TagWidget {
    explicit TagWidget(uint64_t idx);

    uint64_t index;
    Gtk::Box container;
    Gtk::Button button;
    Gtk::Label label;
    Gtk::Box taskbar;
    std::vector<TaskbarWindow> taskbar_windows;
  };

  void onEvent(const Json::Value& ev) override;
  void doUpdate();

  TagWidget& addTag(uint64_t idx);
  void updateTag(TagWidget& widget, const Json::Value& tag, const Json::Value& monitor,
                 const std::vector<Json::Value>& clients);
  void updateTaskbar(TagWidget& widget, const std::vector<Json::Value>& clients);
  Glib::RefPtr<Gio::Icon> resolveIcon(const std::string& app_id, const std::string& title);
  std::string getIcon(const std::string& value, const Json::Value& tag);
  bool handleButtonClick(GdkEventButton* event, uint64_t idx, bool isOverview);

  const Bar& bar_;
  Gtk::Box box_;

  std::unordered_map<uint64_t, std::unique_ptr<TagWidget>> tags_;
  Gtk::Button* overview_button_ = nullptr;

  std::string on_click_left_;
  std::string on_click_middle_;
  std::string on_click_right_;
};

}  // namespace waybar::modules::mango
