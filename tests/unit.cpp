#include "cppbuild/build.hpp"
#include "cppbuild/hash.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
unsigned checks = 0;
void check(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class Function> void rejects(Function function, const std::string& message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, message);
}
void hashes() {
    check(cppbuild::hash("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "empty SHA-256");
    check(cppbuild::hash("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "abc SHA-256");
    check(cppbuild::hash(std::string(1000000, 'a')) ==
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "multi-block SHA-256");
    cppbuild::Sha256 chunks;
    for (unsigned i = 0; i < 1000; ++i) {
        chunks.update(std::string(1000, 'a'));
    }
    check(chunks.finish() == cppbuild::hash(std::string(1000000, 'a')), "incremental hash updates");
}
void graphs() {
    std::mt19937 random(20261101);
    for (unsigned round = 0; round < 80; ++round) {
        std::vector<cppbuild::Node> nodes(40);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            nodes[i].id = std::to_string(i);
            for (std::size_t j = 0; j < i; ++j) {
                if (random() % 13 == 0) {
                    nodes[i].dependencies.push_back(j);
                }
            }
        }
        auto order = cppbuild::topological_order(nodes);
        std::vector<std::size_t> positions(nodes.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            positions[order[i]] = i;
        }
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            for (auto d : nodes[i].dependencies) {
                check(positions[d] < positions[i], "topological edge ordering");
            }
        }
        std::vector<std::atomic<bool>> done(nodes.size());
        for (auto& value : done) {
            value = false;
        }
        std::atomic<bool> stop{false}, violation{false};
        auto result = cppbuild::schedule(nodes, {4, false}, stop, [&](std::size_t id) {
            for (auto d : nodes[id].dependencies) {
                if (!done[d]) {
                    violation = true;
                }
            }
            done[id] = true;
            cppbuild::ActionResult action;
            action.status = "built";
            return action;
        });
        check(result.success && !violation, "scheduler respects dependencies");
        check(result.peak_workers <= 4, "worker limit");
        for (const auto& value : done) {
            check(value, "all scheduled actions complete");
        }
    }
    std::vector<cppbuild::Node> cycle(2);
    cycle[0].id = "a";
    cycle[1].id = "b";
    cycle[0].dependencies = {1};
    cycle[1].dependencies = {0};
    rejects(
        [&] {
            cppbuild::topological_order(cycle);
        },
        "cycles rejected");
    cycle[0].dependencies = {2};
    rejects(
        [&] {
            cppbuild::topological_order(cycle);
        },
        "invalid edge rejected");
}
void failures() {
    std::vector<cppbuild::Node> nodes(5);
    nodes[2].dependencies = {0};
    nodes[3].dependencies = {2};
    nodes[4].dependencies = {1};
    std::atomic<bool> stop{false};
    auto execute = [](std::size_t id) {
        cppbuild::ActionResult result;
        result.status = id == 0 ? "failed" : "built";
        return result;
    };
    auto kept = cppbuild::schedule(nodes, {2, true}, stop, execute);
    check(!kept.success && kept.actions[0].status == "failed", "action failure reported");
    check(kept.actions[2].status == "blocked" && kept.actions[3].status == "blocked",
          "failure propagates to descendants");
    check(kept.actions[1].status == "built" && kept.actions[4].status == "built",
          "keep-going completes independent branch");
    auto fast = cppbuild::schedule(nodes, {1, false}, stop, execute);
    check(fast.actions[1].status == "blocked" && fast.actions[4].status == "blocked",
          "fail-fast prevents new independent work");
    stop = true;
    auto cancelled = cppbuild::schedule(nodes, {3, true}, stop, execute);
    check(!cancelled.success && cancelled.peak_workers == 0,
          "pre-cancelled graph launches nothing");
    stop = false;
    rejects(
        [&] {
            cppbuild::schedule(nodes, {0, false}, stop, execute);
        },
        "invalid jobs rejected");
}
void parallel_workers() {
    std::vector<cppbuild::Node> nodes(4);
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable changed;
    unsigned entered = 0;
    auto result = cppbuild::schedule(nodes, {4, false}, stop, [&](std::size_t) {
        std::unique_lock<std::mutex> lock(mutex);
        ++entered;
        changed.notify_all();
        if (!changed.wait_for(lock, std::chrono::seconds(3), [&] {
                return entered == 4;
            })) {
            throw std::runtime_error("workers failed to overlap");
        }
        cppbuild::ActionResult action;
        action.status = "built";
        return action;
    });
    check(result.success && result.peak_workers == 4, "four real workers overlap");
}
void dependency_files() {
    auto path = cppbuild::fs::temp_directory_path() /
                ("cppbuild-unit-" + std::to_string(::getpid()) + ".d");
    {
        std::ofstream output(path);
        output << "cppbuild: /tmp/source.cpp /tmp/space\\ name.hpp \\\n /tmp/hash\\#name.hpp "
                  "/tmp/dollar$$name.hpp /tmp/space\\ name.hpp\n";
    }
    auto files = cppbuild::read_dependencies(path);
    std::set<cppbuild::fs::path> paths(files.begin(), files.end());
    check(paths.count(cppbuild::fs::weakly_canonical("/tmp/space name.hpp")) &&
              paths.count(cppbuild::fs::weakly_canonical("/tmp/hash#name.hpp")) &&
              paths.count(cppbuild::fs::weakly_canonical("/tmp/dollar$name.hpp")),
          "makefile dependency escapes");
    check(paths.size() == 4, "dependency paths deduplicated");
    {
        std::ofstream output(path);
        output << "other: ignored";
    }
    rejects(
        [&] {
            cppbuild::read_dependencies(path);
        },
        "invalid dependency target rejected");
    cppbuild::fs::remove(path);
    check(cppbuild::json_string("a\n\"b\\c") == "\"a\\u000a\\\"b\\\\c\"", "JSON escaping");
}
} // namespace
int main() {
    try {
        hashes();
        graphs();
        failures();
        parallel_workers();
        dependency_files();
        std::cout << checks << " checks passed; graph seed 20261101\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
