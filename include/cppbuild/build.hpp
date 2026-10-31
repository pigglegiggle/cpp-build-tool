#pragma once
#include <atomic>
#include <csignal>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cppbuild {
namespace fs = std::filesystem;
struct Target {
    std::string name;
    bool library = false;
    std::vector<fs::path> sources;
    std::vector<std::string> dependencies;
};
struct Project {
    fs::path root;
    std::string compiler = "clang++", archiver = "ar";
    std::vector<std::string> flags{"-std=c++17", "-O2"}, link_flags;
    std::vector<fs::path> includes;
    std::vector<Target> targets;
};
Project read_project(const fs::path& file);
std::vector<fs::path> read_dependencies(const fs::path& file);
struct Node {
    std::string id;
    std::vector<std::size_t> dependencies;
    fs::path output;
    fs::path source;
    std::size_t target = 0;
};
std::vector<std::size_t> topological_order(const std::vector<Node>& nodes);
std::vector<Node> graph(const Project& project, const std::string& requested);
std::string json_string(const std::string& text);
std::string graph_json(const std::vector<Node>& nodes);
std::string graph_dot(const std::vector<Node>& nodes);
struct ProcessResult {
    int exit_code = 0;
    std::string output;
    bool truncated = false;
};
ProcessResult process(const std::vector<std::string>& arguments, std::atomic<bool>& stop);
fs::path executable(const std::string& name);
struct ActionResult {
    std::string status = "pending", reason, error;
    double milliseconds = 0;
    unsigned scans = 0, commands = 0;
};
struct BuildOptions {
    unsigned jobs = 1;
    bool keep_going = false;
};
struct BuildReport {
    std::vector<ActionResult> actions;
    double milliseconds = 0;
    unsigned peak_workers = 0;
    bool success = false;
};
BuildReport schedule(const std::vector<Node>& nodes, BuildOptions options, std::atomic<bool>& stop,
                     const std::function<ActionResult(std::size_t)>& execute);
class Workspace {
  public:
    explicit Workspace(const fs::path& root);
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    fs::path path;
    void clean(bool all);

  private:
    int lock_ = -1;
};
BuildReport build(const Project& project, const std::vector<Node>& nodes, Workspace& workspace,
                  BuildOptions options, std::atomic<bool>& stop);
std::string report_json(const std::vector<Node>& nodes, const BuildReport& report);
void atomic_write(const fs::path& file, const std::string& content);
} // namespace cppbuild
