#include "cppbuild/build.hpp"
#include "cppbuild/hash.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cppbuild {
namespace {
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
fs::path local_path(const Project &project, const std::string &value) {
    require(!value.empty() && value.find_first_of("\r\n\t") == std::string::npos &&
                value.find('\0') == std::string::npos,
            "invalid project path");
    fs::path input(value);
    require(!input.is_absolute(), "manifest paths must be relative to the project");
    auto result = fs::weakly_canonical(project.root / input);
    auto relative = result.lexically_relative(project.root);
    require(!relative.empty() && *relative.begin() != ".." && *relative.begin() != ".cppbuild",
            "path escapes project or uses reserved .cppbuild: " + value);
    return result;
}
std::vector<std::string> arguments(std::istringstream &line) {
    std::vector<std::string> result;
    while (line >> std::ws && line.peek() != std::char_traits<char>::eof() && line.peek() != '#') {
        require(line.peek() == '"', "arguments must use double quotes");
        std::string value;
        require(static_cast<bool>(line >> std::quoted(value)), "unterminated quoted argument");
        require(value.find('\0') == std::string::npos &&
                    value.find_first_of("\r\n") == std::string::npos,
                "control characters are not allowed");
        result.push_back(value);
    }
    return result;
}
void validate_flags(const std::vector<std::string> &flags) {
    for (const auto &flag : flags) {
        require(!flag.empty(), "empty compiler flag");
        require(flag[0] != '@' && flag != "-c" && flag.rfind("-o", 0) != 0 &&
                    flag.rfind("-M", 0) != 0,
                "output, dependency and response-file flags are managed by cppbuild: " + flag);
    }
}
} // namespace
Project read_project(const fs::path &file) {
    require(fs::is_regular_file(file) && fs::file_size(file) <= 1024 * 1024,
            "manifest must be a regular file of at most 1 MiB");
    Project project;
    project.root = fs::canonical(file).parent_path();
    std::ifstream input(file);
    require(static_cast<bool>(input), "cannot open manifest");
    Target *current = nullptr;
    std::set<std::string> directives;
    std::string text;
    unsigned number = 0;
    while (std::getline(input, text)) {
        ++number;
        try {
            std::istringstream line(text);
            line >> std::ws;
            if (line.eof() || line.peek() == '#')
                continue;
            std::string directive;
            line >> directive;
            auto values = arguments(line);
            if (directive == "program" || directive == "library") {
                require(values.size() == 1, "target needs one name");
                const auto &name = values[0];
                require(!name.empty() && name.size() <= 64 &&
                            std::isalpha(static_cast<unsigned char>(name[0])),
                        "invalid target name");
                require(std::all_of(name.begin(), name.end(),
                                    [](unsigned char c) {
                                        return std::isalnum(c) || c == '_' || c == '-';
                                    }),
                        "invalid target name");
                require(std::none_of(project.targets.begin(), project.targets.end(),
                                     [&](const Target &t) { return t.name == name; }),
                        "duplicate target: " + name);
                project.targets.push_back({name, directive == "library", {}, {}});
                current = &project.targets.back();
                require(project.targets.size() <= 256, "at most 256 targets");
            } else if (directive == "sources" || directive == "depends") {
                require(current != nullptr && !values.empty(),
                        "sources/depends must follow a target and contain entries");
                if (directive == "depends")
                    current->dependencies.insert(current->dependencies.end(), values.begin(),
                                                 values.end());
                else
                    for (const auto &value : values) {
                        auto source = local_path(project, value);
                        require(fs::is_regular_file(source), "source file not found: " + value);
                        require(source.extension() == ".cpp" || source.extension() == ".cc" ||
                                    source.extension() == ".cxx",
                                "C++ sources must use .cpp, .cc or .cxx");
                        current->sources.push_back(source);
                    }
            } else {
                require(current == nullptr, "project settings must precede target declarations");
                require(directives.insert(directive).second,
                        "duplicate project setting: " + directive);
                if (directive == "compiler" || directive == "archiver") {
                    require(values.size() == 1 && !values[0].empty(),
                            "tool setting needs one executable");
                    (directive == "compiler" ? project.compiler : project.archiver) = values[0];
                } else if (directive == "flags" || directive == "link_flags") {
                    validate_flags(values);
                    (directive == "flags" ? project.flags : project.link_flags) = values;
                } else if (directive == "includes") {
                    for (const auto &value : values) {
                        auto path = local_path(project, value);
                        require(fs::is_directory(path), "include directory not found: " + value);
                        project.includes.push_back(path);
                    }
                } else
                    throw std::runtime_error("unknown directive: " + directive);
            }
        } catch (const std::exception &error) {
            throw std::runtime_error(file.string() + ":" + std::to_string(number) + ": " +
                                     error.what());
        }
    }
    require(!input.bad(), "manifest read failed");
    require(!project.targets.empty(), "manifest needs at least one target");
    std::set<fs::path> all_sources;
    for (const auto &target : project.targets) {
        require(!target.sources.empty(), "target has no sources: " + target.name);
        std::set<fs::path> unique(target.sources.begin(), target.sources.end());
        require(unique.size() == target.sources.size(),
                "duplicate source in target: " + target.name);
        all_sources.insert(unique.begin(), unique.end());
        for (const auto &dependency : target.dependencies) {
            auto found = std::find_if(project.targets.begin(), project.targets.end(),
                                      [&](const Target &t) { return t.name == dependency; });
            require(found != project.targets.end(), "unknown dependency: " + dependency);
            require(found->library,
                    "only static libraries may be target dependencies: " + dependency);
        }
    }
    require(all_sources.size() <= 4096, "at most 4096 distinct source files");
    return project;
}
std::vector<fs::path> read_dependencies(const fs::path &file) {
    require(fs::file_size(file) <= 8 * 1024 * 1024, "dependency file exceeds 8 MiB");
    std::ifstream input(file, std::ios::binary);
    require(static_cast<bool>(input), "dependency file missing");
    std::string text((std::istreambuf_iterator<char>(input)), {});
    require(!input.bad() && text.rfind("cppbuild:", 0) == 0, "invalid compiler dependency file");
    std::set<fs::path> paths;
    std::string word;
    auto flush = [&] {
        if (!word.empty()) {
            paths.insert(fs::weakly_canonical(word));
            word.clear();
        }
    };
    for (std::size_t i = 9; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\\') {
            require(++i < text.size(), "dangling dependency escape");
            if (text[i] == '\n')
                continue;
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
                continue;
            }
            word += text[i];
        } else if (c == '$' && i + 1 < text.size() && text[i + 1] == '$') {
            word += '$';
            ++i;
        } else if (std::isspace(static_cast<unsigned char>(c)))
            flush();
        else
            word += c;
    }
    flush();
    require(!paths.empty(), "compiler reported no dependencies");
    return {paths.begin(), paths.end()};
}
std::vector<std::size_t> topological_order(const std::vector<Node> &nodes) {
    std::vector<std::vector<std::size_t>> users(nodes.size());
    std::vector<std::size_t> remaining(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        std::set<std::size_t> seen;
        for (auto dependency : nodes[i].dependencies) {
            require(dependency < nodes.size() && seen.insert(dependency).second,
                    "invalid or duplicate dependency edge");
            users[dependency].push_back(i);
            ++remaining[i];
        }
    }
    std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<std::size_t>> ready;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (!remaining[i])
            ready.push(i);
    std::vector<std::size_t> result;
    while (!ready.empty()) {
        auto node = ready.top();
        ready.pop();
        result.push_back(node);
        for (auto user : users[node])
            if (--remaining[user] == 0)
                ready.push(user);
    }
    if (result.size() != nodes.size()) {
        std::string message = "dependency cycle involving:";
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (remaining[i])
                message += " " + nodes[i].id;
        throw std::runtime_error(message);
    }
    return result;
}
std::vector<Node> graph(const Project &project, const std::string &requested) {
    std::map<std::string, std::size_t> target_ids;
    for (std::size_t i = 0; i < project.targets.size(); ++i)
        target_ids[project.targets[i].name] = i;
    std::vector<Node> target_graph;
    for (const auto &target : project.targets) {
        Node node;
        node.id = target.name;
        std::set<std::size_t> deps;
        for (const auto &name : target.dependencies)
            deps.insert(target_ids.at(name));
        node.dependencies = {deps.begin(), deps.end()};
        target_graph.push_back(std::move(node));
    }
    auto order = topological_order(target_graph);
    std::set<std::size_t> selected;
    std::function<void(std::size_t)> select = [&](std::size_t i) {
        if (!selected.insert(i).second)
            return;
        for (auto d : target_graph[i].dependencies)
            select(d);
    };
    if (requested == "all")
        for (std::size_t i = 0; i < project.targets.size(); ++i)
            select(i);
    else {
        auto default_target = std::find_if(project.targets.begin(), project.targets.end(),
                                           [](const Target &target) { return !target.library; });
        auto name = requested.empty()
                        ? (default_target == project.targets.end() ? project.targets.front().name
                                                                   : default_target->name)
                        : requested;
        auto found = target_ids.find(name);
        require(found != target_ids.end(), "unknown target: " + name);
        select(found->second);
    }
    std::vector<Node> nodes;
    std::map<fs::path, std::size_t> objects;
    std::map<std::size_t, std::size_t> artifacts;
    for (auto i : order)
        if (selected.count(i)) {
            const auto &target = project.targets[i];
            Node artifact;
            artifact.id = (target.library ? "archive:" : "link:") + target.name;
            artifact.target = i;
            artifact.output = project.root / ".cppbuild" / (target.library ? "lib" : "bin") /
                              (target.library ? "lib" + target.name + ".a" : target.name);
            for (const auto &source : target.sources) {
                if (!objects.count(source)) {
                    Node object;
                    object.id =
                        "compile:" + source.lexically_relative(project.root).generic_string();
                    object.source = source;
                    object.output =
                        project.root / ".cppbuild/objects" / (hash(source.generic_string()) + ".o");
                    objects[source] = nodes.size();
                    nodes.push_back(std::move(object));
                }
                artifact.dependencies.push_back(objects.at(source));
            }
            if (!target.library) {
                std::set<std::size_t> closure;
                std::function<void(std::size_t)> collect = [&](std::size_t id) {
                    for (auto d : target_graph[id].dependencies)
                        if (closure.insert(d).second)
                            collect(d);
                };
                collect(i);
                // Link dependents before their static-library dependencies for one-pass linkers.
                for (auto it = order.rbegin(); it != order.rend(); ++it)
                    if (closure.count(*it))
                        artifact.dependencies.push_back(artifacts.at(*it));
            }
            artifacts[i] = nodes.size();
            nodes.push_back(std::move(artifact));
        }
    topological_order(nodes);
    return nodes;
}
std::string json_string(const std::string &text) {
    std::ostringstream output;
    output << '"';
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\')
            output << '\\' << c;
        else if (c < 32)
            output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned>(c) << std::dec;
        else if (c < 128)
            output << c;
        else {
            unsigned length = c >= 0xc2 && c <= 0xdf   ? 2
                              : c >= 0xe0 && c <= 0xef ? 3
                              : c >= 0xf0 && c <= 0xf4 ? 4
                                                       : 0;
            bool valid = length && i + length <= text.size();
            std::uint32_t code = c & (length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
            for (unsigned j = 1; valid && j < length; ++j) {
                auto next = static_cast<unsigned char>(text[i + j]);
                valid = (next & 0xc0) == 0x80;
                code = (code << 6) | (next & 0x3f);
            }
            valid = valid && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff) &&
                    !(length == 3 && code < 0x800) && !(length == 4 && code < 0x10000);
            if (valid) {
                output.write(text.data() + i, length);
                i += length - 1;
            } else
                output << "\\ufffd";
        }
    }
    output << '"';
    return output.str();
}
std::string graph_json(const std::vector<Node> &nodes) {
    std::ostringstream output;
    output << "{\"nodes\":[";
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i)
            output << ',';
        output << "{\"id\":" << json_string(nodes[i].id)
               << ",\"output\":" << json_string(nodes[i].output.string()) << ",\"dependencies\":[";
        for (std::size_t j = 0; j < nodes[i].dependencies.size(); ++j) {
            if (j)
                output << ',';
            output << json_string(nodes[nodes[i].dependencies[j]].id);
        }
        output << "]}";
    }
    output << "]}\n";
    return output.str();
}
std::string graph_dot(const std::vector<Node> &nodes) {
    std::ostringstream output;
    output << "digraph build {\n  rankdir=LR;\n";
    for (const auto &node : nodes) {
        output << "  " << json_string(node.id) << ";\n";
        for (auto d : node.dependencies)
            output << "  " << json_string(nodes[d].id) << " -> " << json_string(node.id) << ";\n";
    }
    output << "}\n";
    return output.str();
}
} // namespace cppbuild
