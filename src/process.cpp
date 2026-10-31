#include "cppbuild/build.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace cppbuild {
namespace {
std::runtime_error system_error(const std::string &context) {
    return std::runtime_error(context + ": " + std::strerror(errno));
}
std::mutex launches;
class Fd {
  public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() {
        if (value_ >= 0)
            ::close(value_);
    }
    int get() const {
        return value_;
    }
    void close() {
        if (value_ >= 0)
            ::close(value_);
        value_ = -1;
    }

  private:
    int value_;
};
} // namespace
fs::path executable(const std::string &name) {
    auto usable = [](const fs::path &file) {
        return fs::is_regular_file(file) && ::access(file.c_str(), X_OK) == 0;
    };
    if (name.find('/') != std::string::npos) {
        if (usable(name))
            return fs::absolute(name).lexically_normal();
        throw std::runtime_error("executable not found: " + name);
    }
    const char *environment = std::getenv("PATH");
    std::string paths = environment ? environment : "/usr/bin:/bin";
    std::size_t offset = 0;
    while (offset <= paths.size()) {
        auto end = paths.find(':', offset);
        if (end == std::string::npos)
            end = paths.size();
        auto directory = paths.substr(offset, end - offset);
        auto file = fs::path(directory.empty() ? "." : directory) / name;
        if (usable(file))
            return fs::absolute(file).lexically_normal();
        if (end == paths.size())
            break;
        offset = end + 1;
    }
    throw std::runtime_error("executable not found in PATH: " + name);
}
ProcessResult process(const std::vector<std::string> &arguments, std::atomic<bool> &stop) {
    if (arguments.empty())
        throw std::runtime_error("empty process command");
    if (stop)
        return {130, "cancelled before process launch", false};
    std::unique_lock<std::mutex> launch(launches);
    int descriptors[2];
    if (::pipe(descriptors) < 0)
        throw system_error("create output pipe");
    Fd reader(descriptors[0]), writer(descriptors[1]);
    for (int fd : descriptors)
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
            throw system_error("mark pipe close-on-exec");
    if (::fcntl(reader.get(), F_SETFL, O_NONBLOCK) < 0)
        throw system_error("mark output nonblocking");
    std::vector<char *> argv;
    for (const auto &argument : arguments)
        argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int error = posix_spawn_file_actions_init(&actions);
    if (error)
        throw std::runtime_error("initialize spawn actions: " + std::string(std::strerror(error)));
    error = posix_spawnattr_init(&attributes);
    if (error) {
        posix_spawn_file_actions_destroy(&actions);
        throw std::runtime_error("initialize spawn attributes");
    }
    auto cleanup = [&] {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
    };
    error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!error)
        error = posix_spawn_file_actions_adddup2(&actions, writer.get(), STDOUT_FILENO);
    if (!error)
        error = posix_spawn_file_actions_adddup2(&actions, writer.get(), STDERR_FILENO);
    if (!error)
        error = posix_spawn_file_actions_addclose(&actions, reader.get());
    if (!error)
        error = posix_spawn_file_actions_addclose(&actions, writer.get());
    if (!error)
        error = posix_spawnattr_setpgroup(&attributes, 0);
    if (!error)
        error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    if (error) {
        cleanup();
        throw std::runtime_error("configure child process: " + std::string(std::strerror(error)));
    }
    pid_t child = 0;
    error = ::posix_spawn(&child, arguments.front().c_str(), &actions, &attributes, argv.data(),
                          environ);
    cleanup();
    writer.close();
    launch.unlock();
    if (error)
        throw std::runtime_error("launch " + arguments.front() + ": " + std::strerror(error));
    ProcessResult result;
    constexpr std::size_t limit = 1024 * 1024;
    bool exited = false, eof = false, terminated = false, killed = false;
    int status = 0;
    auto terminate_at = std::chrono::steady_clock::time_point{};
    try {
        while (!exited || !eof) {
            auto now = std::chrono::steady_clock::now();
            if (stop && !terminated) {
                ::kill(-child, SIGTERM);
                terminated = true;
                terminate_at = now;
            }
            if (terminated && !killed && now - terminate_at > std::chrono::seconds(1)) {
                ::kill(-child, SIGKILL);
                killed = true;
            }
            pollfd ready{eof ? -1 : reader.get(), POLLIN, 0};
            auto count = ::poll(&ready, 1, 50);
            if (count < 0 && errno != EINTR)
                throw system_error("poll child output");
            std::array<char, 8192> buffer{};
            std::size_t drained = 0;
            while (!eof && drained < 262144) {
                auto size = ::read(reader.get(), buffer.data(), buffer.size());
                if (size > 0) {
                    drained += static_cast<std::size_t>(size);
                    auto keep = std::min<std::size_t>(static_cast<std::size_t>(size),
                                                      limit - result.output.size());
                    result.output.append(buffer.data(), keep);
                    if (keep < static_cast<std::size_t>(size))
                        result.truncated = true;
                } else if (size == 0)
                    eof = true;
                else if (errno == EINTR)
                    continue;
                else if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                else
                    throw system_error("read child output");
            }
            if (!exited) {
                auto waited = ::waitpid(child, &status, WNOHANG);
                if (waited == child)
                    exited = true;
                else if (waited < 0 && errno != EINTR)
                    throw system_error("wait for child");
            }
        }
    } catch (...) {
        ::kill(-child, SIGKILL);
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        throw;
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                         : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return result;
}
} // namespace cppbuild
