#include "modules/mango/backend.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#include "util/scoped_fd.hpp"

namespace waybar::modules::mango {

int IPC::connectToSocket() {
  const char* socket_path = getenv("MANGO_INSTANCE_SIGNATURE");
  if (!socket_path) {
    throw std::runtime_error("Mango IPC: MANGO_INSTANCE_SIGNATURE not set");
  }

  struct sockaddr_un addr;
  util::ScopedFd fd(socket(AF_UNIX, SOCK_STREAM, 0));
  if (fd == -1) throw std::runtime_error("socket() failed");

  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = 0;

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    throw std::runtime_error("connect() failed");
  }
  return fd.release();
}

Json::Value IPC::sendCommand(const std::string& cmd) {
  util::ScopedFd fd(IPC::connectToSocket());
  std::string full_cmd = cmd + "\n";

  ssize_t total_written = 0;
  while (total_written < (ssize_t)full_cmd.size()) {
    ssize_t res = write(fd, full_cmd.c_str() + total_written, full_cmd.size() - total_written);
    if (res < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("Failed to write command");
    }
    total_written += res;
  }

  char buf[4096];
  std::string response;
  while (true) {
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
      if (n == 0) break;
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      throw std::runtime_error("Read error");
    }
    buf[n] = '\0';
    response += buf;
    if (response.find('\n') != std::string::npos) break;
  }

  Json::Value root;
  std::istringstream iss(response);
  Json::CharReaderBuilder builder;
  std::string errors;
  if (!Json::parseFromStream(builder, iss, &root, &errors)) {
    throw std::runtime_error("JSON parse error: " + errors);
  }
  return root;
}

Json::Value IPC::send(const Json::Value& request) {
  if (!request.isMember("command")) {
    throw std::runtime_error("Mango IPC: request must have 'command' field");
  }
  return sendCommand(request["command"].asString());
}

void IPC::sendAsync(const Json::Value& request) {
  if (!request.isMember("command")) {
    spdlog::error("Mango IPC: request must have 'command' field");
    return;
  }
  std::string cmd = request["command"].asString();

  std::thread([cmd]() {
    try {
      IPC::sendCommand(cmd);
    } catch (const std::exception& e) {
      spdlog::error("IPC async send failed: {}", e.what());
    }
  }).detach();
}

IPC::IPC() : active_client_(Json::nullValue) { startIPC(); }

IPC::~IPC() {
  running_ = false;
  if (sockfd_ != -1) shutdown(sockfd_, SHUT_RDWR);
  if (clients_sockfd_ != -1) shutdown(clients_sockfd_, SHUT_RDWR);
  if (ipc_thread_.joinable()) ipc_thread_.join();
  if (clients_ipc_thread_.joinable()) clients_ipc_thread_.join();
  if (sockfd_ != -1) close(sockfd_);
  if (clients_sockfd_ != -1) close(clients_sockfd_);
}

void IPC::startIPC() {
  // Connect synchronously so a missing socket (this WM isn't the active
  // compositor) throws here and lets the module constructor fail, instead of
  // the module always attaching with a permanently empty widget.
  sockfd_ = IPC::connectToSocket();
  try {
    clients_sockfd_ = IPC::connectToSocket();
  } catch (...) {
    close(sockfd_);
    sockfd_ = -1;
    throw;
  }

  ipc_thread_ = std::thread([this] { watchIPC("watch all-monitors", sockfd_); });
  clients_ipc_thread_ = std::thread([this] { watchIPC("watch all-clients", clients_sockfd_); });
}

void IPC::watchIPC(const std::string& subscription, int& socket_fd) {
  spdlog::info("Mango IPC stream started: {}", subscription);

  char buf[4096];
  std::string buffer;
  bool have_initial_fd = true;

  while (running_) {
    if (!have_initial_fd) {
      try {
        socket_fd = IPC::connectToSocket();
      } catch (const std::exception& e) {
        spdlog::error("Mango IPC: failed to reconnect {}: {}", subscription, e.what());
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }
    }
    have_initial_fd = false;

    if (write(socket_fd, subscription.c_str(), subscription.size()) !=
            static_cast<ssize_t>(subscription.size()) ||
        write(socket_fd, "\n", 1) != 1) {
      spdlog::error("Failed to subscribe to {}", subscription);
      if (socket_fd != -1) {
        close(socket_fd);
        socket_fd = -1;
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
      continue;
    }

    struct pollfd pfd;
    pfd.fd = socket_fd;
    pfd.events = POLLIN;
    buffer.clear();

    bool connected = true;
    while (running_ && connected) {
      int ret = poll(&pfd, 1, 1000);
      if (ret == 0) continue;
      if (ret < 0) {
        if (errno == EINTR) continue;
        spdlog::error("IPC poll error for {}: {}", subscription, strerror(errno));
        connected = false;
        break;
      }

      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        connected = false;
        break;
      }

      if (pfd.revents & POLLIN) {
        ssize_t size = read(socket_fd, buf, sizeof(buf));
        if (size == 0) {
          connected = false;
          break;
        }
        if (size < 0) {
          if (errno == EINTR) continue;
          spdlog::error("IPC read error for {}: {}", subscription, strerror(errno));
          connected = false;
          break;
        }
        buffer.append(buf, size);

        size_t position;
        while ((position = buffer.find('\n')) != std::string::npos) {
          std::string line = buffer.substr(0, position);
          buffer.erase(0, position + 1);
          if (line.empty()) continue;
          try {
            parseIPC(line);
          } catch (const std::exception& e) {
            spdlog::warn("Failed to parse IPC line: {} - {}", line, e.what());
          }
        }
      }
    }

    if (!running_) break;
    if (socket_fd != -1) {
      close(socket_fd);
      socket_fd = -1;
    }
    spdlog::warn("Mango IPC stream closed, reconnecting: {}", subscription);
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }

  spdlog::info("Mango IPC stream stopping: {}", subscription);
}

void IPC::parseIPC(const std::string& line) {
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream iss(line);
  if (!Json::parseFromStream(builder, iss, &root, &errors)) {
    throw std::runtime_error("JSON parse error: " + errors);
  }

  if (root.isMember("monitors") && root["monitors"].isArray()) {
    for (const auto& mon : root["monitors"]) {
      handleMonitorUpdate(mon);
    }

    Json::Value active_monitor;
    for (const auto& mon : root["monitors"]) {
      if (mon["active"].asBool()) {
        active_monitor = mon;
        break;
      }
    }

    if (!active_monitor.isNull()) {
      const auto& active_client = active_monitor["active_client"];
      updateFocusingClient(active_client);

      if (active_monitor.isMember("keyboardlayout")) {
        updateKeyboardLayout(active_monitor["keyboardlayout"].asString());
      }

      if (active_monitor.isMember("keymode")) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        keymode_ = active_monitor["keymode"].asString();
      }
    }

    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      for (auto& [ev, handler] : callbacks_) {
        if (ev == "monitor") {
          handler->onEvent(root);
        }
      }
    }

    return;
  }

  if (root.isMember("clients") && root["clients"].isArray()) {
    {
      std::lock_guard<std::mutex> lock(data_mutex_);
      clients_.clear();
      for (const auto& client : root["clients"]) {
        clients_[client["id"].asUInt64()] = client;
      }
    }

    std::lock_guard<std::mutex> lock(callback_mutex_);
    for (auto& [ev, handler] : callbacks_) {
      if (ev == "client") handler->onEvent(root);
    }
    return;
  }

  spdlog::debug("Unhandled IPC message: {}", line);
}

std::unordered_map<std::string, Json::Value> IPC::getMonitors() const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  return monitors_;
}

std::vector<Json::Value> IPC::getClients() const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  std::vector<Json::Value> clients;
  clients.reserve(clients_.size());
  for (const auto& [id, client] : clients_) clients.push_back(client);
  return clients;
}

IPC& IPC::getInstance() {
  static IPC instance;
  return instance;
}

Json::Value IPC::getMonitor(const std::string& name) {
  std::lock_guard<std::mutex> lock(data_mutex_);
  auto it = monitors_.find(name);
  if (it != monitors_.end()) {
    return it->second;
  }
  return Json::nullValue;
}

std::string IPC::getKeyboardLayout() const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  return keyboard_layout_;
}

std::string IPC::getKeymode() const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  return keymode_;
}

Json::Value IPC::getActiveClientForMonitor(const std::string& name) const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  auto it = monitors_.find(name);
  if (it != monitors_.end() && it->second.isMember("active_client")) {
    return it->second["active_client"];
  }
  return Json::nullValue;
}

std::string IPC::getLayoutSymbolForMonitor(const std::string& name) const {
  std::lock_guard<std::mutex> lock(data_mutex_);
  auto it = monitors_.find(name);
  if (it != monitors_.end() && it->second.isMember("layout_symbol")) {
    return it->second["layout_symbol"].asString();
  }
  return {};
}

void IPC::handleMonitorUpdate(const Json::Value& mon) {
  std::lock_guard<std::mutex> lock(data_mutex_);
  monitors_[mon["name"].asString()] = mon;
}

void IPC::updateFocusingClient(const Json::Value& client) {
  {
    std::lock_guard<std::mutex> lock(data_mutex_);
    active_client_ = client;

    if (client.isNull() || !client.isObject() || client["id"].isNull()) {
      focusing_client_id_ = 0;
    } else {
      focusing_client_id_ = client["id"].asUInt64();
    }
  }
}

void IPC::updateKeyboardLayout(const std::string& layout) {
  {
    std::lock_guard<std::mutex> lock(data_mutex_);
    keyboard_layout_ = layout;
  }
}

void IPC::registerForIPC(const std::string& ev, EventHandler* handler) {
  if (!handler) return;
  std::lock_guard<std::mutex> lock(callback_mutex_);
  callbacks_.emplace_back(ev, handler);
}

void IPC::unregisterForIPC(EventHandler* handler) {
  if (!handler) return;
  std::lock_guard<std::mutex> lock(callback_mutex_);
  for (auto it = callbacks_.begin(); it != callbacks_.end();) {
    if (it->second == handler)
      it = callbacks_.erase(it);
    else
      ++it;
  }
}

}  // namespace waybar::modules::mango
