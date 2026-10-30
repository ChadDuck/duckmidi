#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

static void write(const fs::path &path, const std::string &text) {
    if (fs::exists(path) && read(path) == text) return;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output || !(output << text)) throw std::runtime_error("Cannot write " + path.string());
}

static std::string attribute(const std::string &tag, const std::string &name) {
    const std::regex pattern("(?:^|\\s)" + name + "\\s*=\\s*\"([^\"]*)\"");
    std::smatch match;
    if (!std::regex_search(tag, match, pattern)) return {};
    return match[1];
}

static double number(const std::string &text) {
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    double value;
    if (!(input >> value) || !std::isfinite(value)) throw std::runtime_error("Invalid SVG number: " + text);
    input >> std::ws;
    if (!input.eof()) throw std::runtime_error("Invalid SVG number: " + text);
    return value;
}

static std::string literal(double value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(6) << value;
    std::string text = output.str();
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text += '0';
    return text + 'f';
}

static void emit(std::ostringstream &output, const std::vector<double> &points, int closed, int icon) {
    std::vector<double> clean;
    for (size_t i = 0; i < points.size(); i += 2) {
        if (clean.size() >= 2 && std::abs(clean[clean.size()-2] - points[i]) < .00001 &&
            std::abs(clean.back() - points[i+1]) < .00001) continue;
        clean.push_back(points[i]);
        clean.push_back(points[i+1]);
    }
    if (closed && clean.size() >= 6 && std::abs(clean[0] - clean[clean.size()-2]) < .00001 &&
        std::abs(clean[1] - clean.back()) < .00001) clean.resize(clean.size()-2);
    if (clean.size() < 4) return;
    if (clean.size() > 512) throw std::runtime_error("Icon path exceeds baked capacity");
    output << "    {" << icon << ", " << closed << ", " << clean.size() << ", {";
    for (size_t i = 0; i < clean.size(); ++i) output << (i ? ", " : "") << literal(clean[i]);
    output << "}},\n";
}

// This is an asset compiler for the repository's absolute M/L/H/V/C/Z paths,
// not a general SVG renderer. Reject unsupported shapes/commands explicitly.
static void path(std::ostringstream &output, const std::string &data, int icon) {
    const std::regex token("[a-zA-Z]|[+-]?(?:[0-9]*\\.[0-9]+|[0-9]+\\.?[0-9]*)(?:[eE][+-]?[0-9]+)?");
    std::vector<std::string> tokens;
    size_t end = 0;
    for (auto it = std::sregex_iterator(data.begin(), data.end(), token); it != std::sregex_iterator(); ++it) {
        const auto begin = static_cast<size_t>(it->position());
        if (data.substr(end, begin-end).find_first_not_of(" \t\r\n,") != std::string::npos)
            throw std::runtime_error("Invalid SVG path syntax");
        tokens.push_back(it->str());
        end = begin + it->length();
    }
    if (data.substr(end).find_first_not_of(" \t\r\n,") != std::string::npos)
        throw std::runtime_error("Invalid SVG path syntax");
    std::vector<double> points;
    double x = 0, y = 0;
    char command = 0;
    for (size_t i = 0; i < tokens.size();) {
        const char first = tokens[i][0];
        if ((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z')) {
            command = first;
            ++i;
        }
        if (command == 'Z') {
            emit(output, points, 1, icon);
            points.clear();
            command = 0;
            continue;
        }
        if (command != 'M' && command != 'L' && command != 'H' && command != 'V' && command != 'C')
            throw std::runtime_error("Unsupported SVG path command");
        const size_t count = command == 'C' ? 6 : command == 'H' || command == 'V' ? 1 : 2;
        if (tokens.size()-i < count) throw std::runtime_error("Incomplete SVG path command");
        double v[6] = {};
        for (size_t j = 0; j < count; ++j) v[j] = number(tokens[i++]);
        if (command == 'M') {
            emit(output, points, 0, icon);
            points.clear();
            x = v[0]; y = v[1]; command = 'L';
            points.insert(points.end(), {x, y});
        } else if (command == 'C') {
            for (int j = 1; j <= 12; ++j) {
                const double t = j / 12.0, u = 1-t;
                points.push_back(u*u*u*x + 3*u*u*t*v[0] + 3*u*t*t*v[2] + t*t*t*v[4]);
                points.push_back(u*u*u*y + 3*u*u*t*v[1] + 3*u*t*t*v[3] + t*t*t*v[5]);
            }
            x = v[4]; y = v[5];
        } else {
            if (command == 'H') x = v[0];
            else if (command == 'V') y = v[0];
            else { x = v[0]; y = v[1]; }
            points.insert(points.end(), {x, y});
        }
    }
    emit(output, points, 0, icon);
}

static std::vector<std::string> shapes(const std::string &svg, const std::string &kind) {
    const std::regex tag("<([A-Za-z][A-Za-z0-9:-]*)\\b[^>]*>");
    std::vector<std::string> result;
    for (auto it = std::sregex_iterator(svg.begin(), svg.end(), tag); it != std::sregex_iterator(); ++it) {
        if ((*it)[1] == "svg") continue;
        if ((*it)[1] != kind) throw std::runtime_error("Unsupported SVG element: " + (*it)[1].str());
        result.push_back(it->str());
    }
    if (result.empty()) throw std::runtime_error("SVG contains no " + kind + " elements");
    return result;
}

static std::string toolbar(const fs::path &root) {
    const char *files[] = {"play", "pause", "square", "circle", "open-select-hand-gesture",
        "square-dashed", "cut-copy", "trash", "design-nib", "circle-spark", "timer", "hourglass",
        "plus-square", "cursor-pointer", "drag-hand-gesture", "one-finger-select-hand-gesture", "piano-outline"};
    std::ostringstream output;
    output << "// Baked from MIT-licensed Iconoir SVGs and local piano outline; see assets/iconoir/LICENSE.\n"
              "struct ToolbarShape { int icon, kind, count; float values[512]; };\n"
              "static const ToolbarShape toolbar_shapes[] = {\n";
    for (size_t i = 0; i < std::size(files); ++i) {
        const bool local = std::string(files[i]) == "cut-copy" || std::string(files[i]) == "piano-outline";
        const fs::path file = root / "assets" / (local ? "" : "iconoir") / (std::string(files[i])+".svg");
        for (const auto &shape : shapes(read(file), "path")) path(output, attribute(shape, "d"), static_cast<int>(i));
    }
    output << "};\n";
    return output.str();
}

static std::string piano(const fs::path &root) {
    std::ostringstream output;
    output << "struct PianoRect { float x, y, w, h, radius; unsigned color; };\n"
              "static const PianoRect piano_rects[] = {\n";
    for (const auto &shape : shapes(read(root / "assets/piano.svg"), "rect")) {
        output << "    {";
        const char *fields[] = {"x", "y", "width", "height", "rx"};
        for (const char *field : fields) {
            const std::string value = attribute(shape, field);
            if (value.empty() && (std::string(field) == "width" || std::string(field) == "height"))
                throw std::runtime_error("Piano rectangle needs width and height");
            output << literal(value.empty() ? 0 : number(value)) << ", ";
        }
        const std::string fill = attribute(shape, "fill");
        if (!std::regex_match(fill, std::regex("#[0-9a-fA-F]{6}")))
            throw std::runtime_error("Piano rectangle needs an RGB fill");
        output << "0xff" << fill.substr(5,2) << fill.substr(3,2) << fill.substr(1,2) << "u},\n";
    }
    output << "};\n";
    return output.str();
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
        if (argc != 3) throw std::runtime_error("Usage: bake-icons <source-root> <output-directory>");
        const fs::path root = argv[1], output = argv[2];
        const std::string toolbar_text = toolbar(root), piano_text = piano(root);
        fs::create_directories(output);
        write(output / "toolbar_icons.h", toolbar_text);
        write(output / "piano_icon.h", piano_text);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Icon generation failed: " << error.what() << '\n';
        return 1;
    }
}
