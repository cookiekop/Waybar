#pragma once

#include <gtkmm/button.h>
#include <json/value.h>

#include <mutex>

#include "AIconLabel.hpp"
#include "bar.hpp"
#include "modules/mango/backend.hpp"

namespace waybar::modules::mango {

class Window : public AIconLabel, public EventHandler {
 public:
  Window(const std::string&, const Bar&, const Json::Value&);
  ~Window() override;
  void update() override;

 private:
  void onEvent(const Json::Value& ev) override;
  void doUpdate();
  void updateAppIcon(const std::string& app_id, const std::string& title);
  void setClass(const std::string& className, bool enable);

  const Bar& bar_;
  std::string oldAppId_;
  std::string iconAppId_;
  std::string iconTitle_;
  std::mutex mutex_;
};

}  // namespace waybar::modules::mango
