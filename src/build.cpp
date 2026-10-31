#include "cppbuild/build.hpp"
#include "cppbuild/hash.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

extern char **environ;
namespace cppbuild {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<unsigned long> sequence{0};
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void directory(const fs::path &path) {
    if (fs::is_symlink(fs::symlink_status(path)))
        throw std::runtime_error("managed directory cannot be a symlink: " + path.string());
    fs::create_directories(path);
    if (!fs::is_directory(path))
        throw std::runtime_error("not a directory: " + path.string());
}
fs::path temporary(const fs::path &file) {
    return file.parent_path() / (file.filename().string() + ".tmp." + std::to_string(::getpid()) +
                                 "." + std::to_string(sequence++));
}
void sync_directory(const fs::path &path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("cannot open directory for sync: " + path.string());
    int error = ::fsync(fd);
    int saved = errno;
    ::close(fd);
    if (error < 0)
        throw std::runtime_error("directory sync failed after replacement: " +
                                 std::string(std::strerror(saved)));
}
void copy_atomic(const fs::path &source, const fs::path &destination) {
    auto temp = temporary(destination);
    try {
        fs::copy_file(source, temp, fs::copy_options::none);
        int fd = ::open(temp.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            throw std::runtime_error("cannot sync copied artifact");
        int error = ::fsync(fd);
        ::close(fd);
        if (error < 0)
            throw std::runtime_error("artifact sync failed");
        fs::rename(temp, destination);
        sync_directory(destination.parent_path());
    } catch (...) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }
}
struct State {
    std::string key, digest;
};
State read_state(const fs::path &path) {
    try {
        if (fs::is_symlink(fs::symlink_status(path)) || !fs::is_regular_file(path) ||
            fs::file_size(path) > 512)
            return {};
        std::ifstream input(path);
        std::string magic;
        State state;
        if (!(std::getline(input, magic)) || magic != "CPPBUILD_STATE_1" ||
            !(input >> std::quoted(state.key) >> std::quoted(state.digest)))
            return {};
        input >> std::ws;
        if (!input.eof() || state.key.size() != 64 || state.digest.size() != 64)
            return {};
        auto valid = [](const std::string &value) {
            return std::all_of(value.begin(), value.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        };
        return valid(state.key) && valid(state.digest) ? state : State{};
    } catch (const fs::filesystem_error &) {
        return {};
    }
}
void write_state(const fs::path &path, const State &state) {
    atomic_write(path, "CPPBUILD_STATE_1\n" + state.key + "\n" + state.digest + "\n");
}
void feed(Sha256 &digest, const std::string &value) {
    digest.update(std::to_string(value.size()) + ":");
    digest.update(value);
}
std::string environment_digest() {
    std::vector<std::string> values;
    for (char **item = environ; *item; ++item) {
        std::string value(*item);
        auto name = value.substr(0, value.find('='));
        if (name != "PWD" && name != "OLDPWD" && name != "SHLVL" && name != "_" &&
            name != "TERM_SESSION_ID")
            values.push_back(value);
    }
    std::sort(values.begin(), values.end());
    Sha256 digest;
    for (const auto &value : values)
        feed(digest, value);
    return digest.finish();
}
std::string tool_digest(const fs::path &tool, std::atomic<bool> &stop) {
    auto version = process({tool.string(), "--version"}, stop);
    // Some POSIX archivers do not support --version; the executable digest still participates.
    return hash(tool.string() + "\n" + hash_file(tool) + "\n" + version.output);
}
std::vector<std::pair<fs::path, std::string>> fingerprints(const std::vector<fs::path> &paths) {
    std::vector<std::pair<fs::path, std::string>> result;
    for (const auto &path : paths)
        result.emplace_back(path, hash_file(path));
    return result;
}
std::string action_key(const Node &node, const std::vector<std::string> &arguments,
                       const std::string &tool, const std::string &environment,
                       const std::vector<std::pair<fs::path, std::string>> &inputs) {
    Sha256 digest;
    feed(digest, "CPPBUILD_ACTION_1");
    feed(digest, node.id);
    feed(digest, tool);
    feed(digest, environment);
    for (const auto &argument : arguments)
        feed(digest, argument);
    for (const auto &input : inputs) {
        feed(digest, input.first.string());
        feed(digest, input.second);
    }
    return digest.finish();
}
std::string command_failure(const std::vector<std::string> &arguments,
                            const ProcessResult &result) {
    std::ostringstream message;
    message << "command exited " << result.exit_code << ":";
    for (const auto &argument : arguments)
        message << ' ' << std::quoted(argument);
    message << '\n' << result.output;
    if (result.truncated)
        message << "\n[diagnostics truncated at 1 MiB]\n";
    return message.str();
}
} // namespace
void atomic_write(const fs::path &file, const std::string &content) {
    auto temp = temporary(file);
    int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot create temporary file: " + temp.string());
    try {
        std::size_t written = 0;
        while (written < content.size()) {
            auto count = ::write(fd, content.data() + written, content.size() - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error("temporary file write failed");
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(fd) < 0)
            throw std::runtime_error("temporary file sync failed");
        ::close(fd);
        fd = -1;
        fs::rename(temp, file);
        sync_directory(file.parent_path());
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw;
    }
}
Workspace::Workspace(const fs::path &root) : path(root / ".cppbuild") {
    directory(path);
    lock_ = ::open((path / "lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0)
        throw std::runtime_error("cannot open workspace lock");
    if (::flock(lock_, LOCK_EX | LOCK_NB) < 0) {
        ::close(lock_);
        lock_ = -1;
        throw std::runtime_error("another build or clean owns this workspace");
    }
    try {
        auto marker = path / "owner";
        std::string expected = "CPPBUILD_WORKSPACE_1\n" + root.string() + "\n";
        if (fs::exists(marker) || fs::is_symlink(fs::symlink_status(marker))) {
            if (fs::is_symlink(fs::symlink_status(marker)) || fs::file_size(marker) > 8192)
                throw std::runtime_error("invalid workspace owner marker");
            std::ifstream input(marker);
            std::string actual((std::istreambuf_iterator<char>(input)), {});
            if (actual != expected)
                throw std::runtime_error("workspace belongs to a different project");
        } else {
            // Refuse to adopt a populated directory that may contain unrelated user files.
            for (const auto &item : fs::directory_iterator(path))
                if (item.path().filename() != "lock")
                    throw std::runtime_error("unrecognized .cppbuild directory");
            atomic_write(marker, expected);
        }
        for (const auto *name : {"objects", "bin", "lib", "state", "cache", "work", "logs"})
            directory(path / name);
    } catch (...) {
        ::close(lock_);
        lock_ = -1;
        throw;
    }
}
Workspace::~Workspace() {
    if (lock_ >= 0)
        ::close(lock_);
}
void Workspace::clean(bool all) {
    // Keep the locked inode and owner marker so a concurrent build cannot acquire a new lock.
    for (const auto *name : {"objects", "bin", "lib", "state", "work", "logs"}) {
        fs::remove_all(path / name);
        directory(path / name);
    }
    if (all) {
        fs::remove_all(path / "cache");
        directory(path / "cache");
    }
    fs::remove(path / "last-build.json");
}
BuildReport schedule(const std::vector<Node> &nodes, BuildOptions options, std::atomic<bool> &stop,
                     const std::function<ActionResult(std::size_t)> &execute) {
    if (options.jobs < 1 || options.jobs > 32)
        throw std::runtime_error("jobs must be 1..32");
    topological_order(nodes);
    BuildReport report;
    report.actions.resize(nodes.size());
    auto started = Clock::now();
    std::vector<std::vector<std::size_t>> users(nodes.size());
    std::vector<std::size_t> prerequisites(nodes.size());
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<std::size_t>> ready;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        prerequisites[i] = nodes[i].dependencies.size();
        for (auto d : nodes[i].dependencies)
            users[d].push_back(i);
        if (!prerequisites[i])
            ready.push(i);
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t left = nodes.size();
    unsigned active = 0;
    auto block = [&](const std::string &reason) {
        for (auto &action : report.actions)
            if (action.status == "pending") {
                action.status = "blocked";
                action.reason = reason;
                --left;
            }
    };
    auto worker = [&] {
        for (;;) {
            std::size_t id;
            {
                std::unique_lock<std::mutex> lock(mutex);
                for (;;) {
                    if (stop)
                        block("cancelled");
                    while (!ready.empty() && report.actions[ready.top()].status != "pending")
                        ready.pop();
                    if (!left)
                        return;
                    if (!ready.empty())
                        break;
                    changed.wait_for(lock, std::chrono::milliseconds(50));
                }
                id = ready.top();
                ready.pop();
                report.actions[id].status = "running";
                report.peak_workers = std::max(report.peak_workers, ++active);
            }
            auto action_started = Clock::now();
            ActionResult result;
            try {
                result = execute(id);
                if (result.status != "built" && result.status != "restored" &&
                    result.status != "up_to_date" && result.status != "failed")
                    throw std::runtime_error("invalid action result");
            } catch (const std::exception &error) {
                result.status = "failed";
                result.error = error.what();
            }
            result.milliseconds = elapsed(action_started);
            {
                std::lock_guard<std::mutex> lock(mutex);
                report.actions[id] = std::move(result);
                --active;
                --left;
                if (report.actions[id].status == "failed") {
                    if (!options.keep_going)
                        block("stopped after a failed action");
                    else {
                        std::vector<std::size_t> stack = users[id];
                        while (!stack.empty()) {
                            auto next = stack.back();
                            stack.pop_back();
                            if (report.actions[next].status != "pending")
                                continue;
                            report.actions[next].status = "blocked";
                            report.actions[next].reason = "dependency failed";
                            --left;
                            stack.insert(stack.end(), users[next].begin(), users[next].end());
                        }
                    }
                } else
                    for (auto user : users[id])
                        if (report.actions[user].status == "pending" && --prerequisites[user] == 0)
                            ready.push(user);
            }
            changed.notify_all();
        }
    };
    std::vector<std::thread> threads;
    try {
        for (unsigned i = 0; i < std::min<std::size_t>(options.jobs, nodes.size()); ++i)
            threads.emplace_back(worker);
    } catch (...) {
        stop = 1;
        changed.notify_all();
        for (auto &thread : threads)
            thread.join();
        throw;
    }
    for (auto &thread : threads)
        thread.join();
    report.milliseconds = elapsed(started);
    report.success =
        !stop &&
        std::none_of(report.actions.begin(), report.actions.end(), [](const ActionResult &result) {
            return result.status == "failed" || result.status == "blocked";
        });
    return report;
}
BuildReport build(const Project &project, const std::vector<Node> &nodes, Workspace &workspace,
                  BuildOptions options, std::atomic<bool> &stop) {
    auto overall = Clock::now();
    auto compiler = executable(project.compiler);
    bool needs_archiver = false;
    for (const auto &node : nodes)
        if (node.source.empty() && project.targets[node.target].library)
            needs_archiver = true;
    auto archiver = needs_archiver ? executable(project.archiver) : fs::path{};
    auto compiler_id = tool_digest(compiler, stop);
    auto archiver_id = needs_archiver ? tool_digest(archiver, stop) : "";
    auto environment = environment_digest();
    std::vector<std::string> common{compiler.string()};
    common.insert(common.end(), project.flags.begin(), project.flags.end());
    for (const auto &include : project.includes) {
        common.push_back("-I");
        common.push_back(include.string());
    }
    auto execute = [&](std::size_t id) {
        const auto &node = nodes[id];
        ActionResult result;
        auto name = hash(node.id);
        auto state_path = workspace.path / "state" / (name + ".state");
        // Unique staging names keep orphaned compiler processes from a killed build isolated.
        auto stage = temporary(workspace.path / "work" / name).string();
        fs::path scan_path = stage + ".scan.d", compile_dep = stage + ".compile.d",
                 staged = stage + ".output";
        struct Scratch {
            std::vector<fs::path> files;
            ~Scratch() {
                for (const auto &file : files) {
                    std::error_code ignored;
                    fs::remove(file, ignored);
                }
            }
        } scratch{{scan_path, compile_dep, staged}};
        std::vector<std::string> command;
        std::vector<fs::path> inputs;
        bool compile = !node.source.empty();
        if (compile) {
            command = common;
            command.insert(command.end(), {"-M", "-MT", "cppbuild", "-MF", scan_path.string(),
                                           node.source.string()});
            auto scan = process(command, stop);
            ++result.scans;
            if (scan.exit_code) {
                result.status = "failed";
                result.error = command_failure(command, scan);
                return result;
            }
            inputs = read_dependencies(scan_path);
            command = common;
            command.insert(command.end(), {"-c", node.source.string(), "-MD", "-MT", "cppbuild",
                                           "-MF", "<DEPS>", "-o", "<OUTPUT>"});
        } else {
            const auto &target = project.targets[node.target];
            command = {target.library ? archiver.string() : compiler.string()};
            if (target.library)
                command.insert(command.end(), {"rcs", "<OUTPUT>"});
            else {
                command.insert(command.end(), project.flags.begin(), project.flags.end());
                command.insert(command.end(), {"-o", "<OUTPUT>"});
            }
            for (auto dependency : node.dependencies) {
                inputs.push_back(nodes[dependency].output);
                command.push_back(nodes[dependency].output.string());
            }
            if (!target.library)
                command.insert(command.end(), project.link_flags.begin(), project.link_flags.end());
        }
        auto before = fingerprints(inputs);
        auto key = action_key(node, command,
                              (!compile && project.targets[node.target].library) ? archiver_id
                                                                                 : compiler_id,
                              environment, before);
        auto state = read_state(state_path);
        auto cache = workspace.path / "cache" / key;
        directory(cache);
        auto cached_state = read_state(cache / "state");
        auto matches = [&](const fs::path &path, const std::string &digest) {
            return !digest.empty() && !fs::is_symlink(fs::symlink_status(path)) &&
                   fs::is_regular_file(path) && hash_file(path) == digest;
        };
        if (state.key == key && matches(node.output, state.digest)) {
            result.status = "up_to_date";
            result.reason = "inputs, command, environment and output content match";
            return result;
        }
        if (cached_state.key == key && matches(cache / "artifact", cached_state.digest)) {
            if (fingerprints(inputs) != before)
                throw std::runtime_error(
                    "inputs changed during cache verification; retry the build");
            copy_atomic(cache / "artifact", node.output);
            write_state(state_path, cached_state);
            result.status = "restored";
            result.reason = "verified action cache hit";
            return result;
        }
        result.reason = state.key.empty()  ? "no valid prior state"
                        : state.key != key ? "inputs, command, tool or environment changed"
                                           : "output is missing or modified";
        std::error_code ignored;
        fs::remove(staged, ignored);
        fs::remove(compile_dep, ignored);
        for (auto &argument : command) {
            if (argument == "<OUTPUT>")
                argument = staged.string();
            if (argument == "<DEPS>")
                argument = compile_dep.string();
        }
        auto compiled = process(command, stop);
        ++result.commands;
        atomic_write(workspace.path / "logs" / (name + ".log"),
                     compiled.output + (compiled.truncated ? "\n[truncated at 1 MiB]\n" : ""));
        if (compiled.exit_code) {
            fs::remove(staged, ignored);
            result.status = "failed";
            result.error = command_failure(command, compiled);
            return result;
        }
        if (stop) {
            fs::remove(staged, ignored);
            result.status = "failed";
            result.error = "cancelled before committing output";
            return result;
        }
        if (compile && read_dependencies(compile_dep) != inputs)
            throw std::runtime_error("dependency set changed during compilation; retry the build");
        if (fingerprints(inputs) != before)
            throw std::runtime_error(
                "input content changed during compilation; prior output preserved");
        auto digest = hash_file(staged);
        copy_atomic(staged, node.output);
        copy_atomic(staged, cache / "artifact");
        State next{key, digest};
        write_state(cache / "state", next);
        write_state(state_path, next);
        fs::remove(staged, ignored);
        result.status = "built";
        return result;
    };
    auto report = schedule(nodes, options, stop, execute);
    report.milliseconds = elapsed(overall);
    atomic_write(workspace.path / "last-build.json", report_json(nodes, report));
    return report;
}
std::string report_json(const std::vector<Node> &nodes, const BuildReport &report) {
    std::ostringstream output;
    unsigned scans = 0, commands = 0, built = 0, restored = 0, unchanged = 0, failed = 0,
             blocked = 0;
    for (const auto &result : report.actions) {
        scans += result.scans;
        commands += result.commands;
        built += result.status == "built";
        restored += result.status == "restored";
        unchanged += result.status == "up_to_date";
        failed += result.status == "failed";
        blocked += result.status == "blocked";
    }
    output << std::setprecision(10) << "{\"success\":" << (report.success ? "true" : "false")
           << ",\"milliseconds\":" << report.milliseconds << ",\"built\":" << built
           << ",\"restored\":" << restored << ",\"up_to_date\":" << unchanged
           << ",\"failed\":" << failed << ",\"blocked\":" << blocked
           << ",\"dependency_scans\":" << scans << ",\"build_commands\":" << commands
           << ",\"peak_workers\":" << report.peak_workers << ",\"actions\":[";
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i)
            output << ',';
        const auto &result = report.actions[i];
        output << "{\"id\":" << json_string(nodes[i].id)
               << ",\"output\":" << json_string(nodes[i].output.string())
               << ",\"status\":" << json_string(result.status)
               << ",\"reason\":" << json_string(result.reason)
               << ",\"error\":" << json_string(result.error)
               << ",\"milliseconds\":" << result.milliseconds
               << ",\"dependency_scans\":" << result.scans
               << ",\"build_commands\":" << result.commands << "}";
    }
    output << "]}\n";
    return output.str();
}
} // namespace cppbuild
