#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "util/app_icon.hpp"

using waybar::util::matchesWebApp;

TEST_CASE("web app identity does not depend on its changing page title", "[app_icon]") {
  REQUIRE(matchesWebApp("chrome-devpod.homepage.watchtower.bond__-Default",
                        "watchtower-browser --app=https://devpod.homepage.watchtower.bond/ "
                        "\"--profile-directory=Default\""));
  REQUIRE(matchesWebApp(
      "chrome-mail.google.com__-Default",
      "watchtower-browser --app=https://mail.google.com --profile-directory=Default"));
  REQUIRE(matchesWebApp("chrome-teams.microsoft.com__v2_-Profile_2",
                        "watchtower-browser --app=https://teams.microsoft.com/v2/ "
                        "--profile-directory 'Profile 2'"));
}

TEST_CASE("web apps with the same URL match the correct Chrome profile", "[app_icon]") {
  const std::string work =
      "watchtower-browser --app=https://app.slack.com/client/ "
      "\"--profile-directory=Profile 2\"";
  REQUIRE(matchesWebApp("chrome-app.slack.com__client_-Profile_2", work));
  REQUIRE_FALSE(matchesWebApp("chrome-app.slack.com__client_-Default", work));
  REQUIRE_FALSE(matchesWebApp("chrome-app.slack.com.evil__client_-Profile_2", work));
}

TEST_CASE("Chrome extension identities and startup classes remain supported", "[app_icon]") {
  REQUIRE(matchesWebApp("chrome-abcdef-Profile_2", "chrome --app-id=abcdef"));
  REQUIRE(matchesWebApp("chrome-abcdef-Default", "launcher", "crx_abcdef"));
  REQUIRE(matchesWebApp("chrome-devpod.homepage.watchtower.bond__-Default", "launcher",
                        "chrome-devpod.homepage.watchtower.bond__-Default"));
  REQUIRE_FALSE(matchesWebApp("org.gnome.Nautilus", "chrome --app-id=abcdef"));
}

TEST_CASE("invalid commands never match a web app", "[app_icon]") {
  REQUIRE_FALSE(matchesWebApp("chrome-host__-Default", "chrome --app='"));
  REQUIRE_FALSE(matchesWebApp("chrome-host__-Default", "chrome --app=invalid"));
  REQUIRE_FALSE(matchesWebApp("chrome-host__-Default", "watchtower-browser"));
}
