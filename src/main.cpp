#include "cppbuild/build.hpp"
#include <algorithm>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<bool> stopped{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal cancellation needs a lock-free flag");
void stop(int) {
    stopped.store(true, std::memory_order_relaxed);
}
void help() {
    std::cout << R"(C++ Build Tool

  cppbuild build [target|all] [--file Cppbuildfile] [--jobs N]
                 [--keep-going] [--explain] [--json]
  cppbuild graph [target|all] [--file Cppbuildfile] [--json]
  cppbuild clean [--file Cppbuildfile] [--all]

Build runs the compiler's dependency scanner, then compiles, restores or reuses
verified artifacts. The default target is the first program (otherwise first library).
Graph prints DOT by default. Clean preserves the action cache unless --all is given.
Manifest compiler commands are trusted executable code; no shell is used.
)";
}
} // namespace
int main(int argc, char **argv) {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        if (argc == 1 || std::string(argv[1]) == "--help") {
            help();
            return 0;
        }
        std::string command = argv[1], target;
        cppbuild::fs::path file = "Cppbuildfile";
        cppbuild::BuildOptions options;
        options.jobs = std::min(32u, std::max(1u, std::thread::hardware_concurrency()));
        bool json = false, explain = false, all = false;
        for (int i = 2; i < argc; ++i) {
            std::string argument = argv[i];
            auto value = [&] {
                if (++i >= argc)
                    throw std::runtime_error("missing value for " + argument);
                return std::string(argv[i]);
            };
            if (argument == "--file")
                file = value();
            else if (argument == "--jobs") {
                auto count = value();
                if (count.empty() || !std::all_of(count.begin(), count.end(),
                                                  [](char c) { return c >= '0' && c <= '9'; }))
                    throw std::runtime_error("jobs must be an integer 1..32");
                auto parsed = std::stoul(count);
                if (parsed < 1 || parsed > 32)
                    throw std::runtime_error("jobs must be 1..32");
                options.jobs = static_cast<unsigned>(parsed);
            } else if (argument == "--keep-going")
                options.keep_going = true;
            else if (argument == "--json")
                json = true;
            else if (argument == "--explain")
                explain = true;
            else if (argument == "--all")
                all = true;
            else if (argument == "--help") {
                help();
                return 0;
            } else if (!argument.empty() && argument[0] != '-' && target.empty())
                target = argument;
            else
                throw std::runtime_error("unknown or repeated argument: " + argument);
        }
        if (command != "build" && command != "graph" && command != "clean")
            throw std::runtime_error("unknown command: " + command);
        if (all && command != "clean")
            throw std::runtime_error("--all is a clean option; use target 'all' for builds");
        if (command == "clean" && (!target.empty() || json || explain || options.keep_going))
            throw std::runtime_error("unsupported clean option");
        if (command != "build" && (explain || options.keep_going))
            throw std::runtime_error("--explain/--keep-going require build");
        if (command == "clean") {
            auto manifest = cppbuild::fs::canonical(file);
            if (!cppbuild::fs::is_regular_file(manifest))
                throw std::runtime_error("manifest must be a regular file");
            // Cleaning needs the workspace location, not valid sources or build instructions.
            cppbuild::Workspace workspace(manifest.parent_path());
            workspace.clean(all);
            std::cout << "Cleaned generated outputs"
                      << (all ? " and action cache" : "; action cache retained") << '\n';
            return 0;
        }
        auto project = cppbuild::read_project(file);
        cppbuild::fs::current_path(project.root);
        auto nodes = cppbuild::graph(project, target);
        if (command == "graph") {
            std::cout << (json ? cppbuild::graph_json(nodes) : cppbuild::graph_dot(nodes));
            return 0;
        }
        cppbuild::Workspace workspace(project.root);
        auto report = cppbuild::build(project, nodes, workspace, options, stopped);
        if (json)
            std::cout << cppbuild::report_json(nodes, report);
        else {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const auto &action = report.actions[i];
                std::cout << action.status << "  " << nodes[i].id;
                if (explain && !action.reason.empty())
                    std::cout << " — " << action.reason;
                std::cout << '\n';
                if (!action.error.empty())
                    std::cerr << nodes[i].id << ": " << action.error << '\n';
            }
            std::cout << (report.success ? "Build succeeded" : "Build failed") << " in "
                      << report.milliseconds << " ms; peak workers " << report.peak_workers << '\n';
            std::cout << "Report: " << (workspace.path / "last-build.json") << '\n';
        }
        return stopped ? 130 : report.success ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "cppbuild: " << error.what() << '\n';
        return stopped ? 130 : 1;
    }
}
