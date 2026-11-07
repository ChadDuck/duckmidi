#include "source.h"
#include "../../src/text/en.h"
#include "internal.h"
#include <pugixml.hpp>
#include <cstring>
#include <new>
#include <string>
#include <vector>

struct XmlEntry {
    pugi::xml_node node;
    EsXmlNode view{};
    std::vector<EsXmlAttribute> attributes;
};

struct EsXml {
    pugi::xml_document document;
    std::string source;
    std::vector<XmlEntry> nodes;
};

static bool gather(EsXml &xml, pugi::xml_node node, size_t parent, unsigned depth) {
    if (depth > 64 || xml.nodes.size() >= 20000) {
        return false;
    }

    size_t index = xml.nodes.size();
    xml.nodes.emplace_back();
    xml.nodes[index].node = node;
    xml.nodes[index].view.name = node.name();
    xml.nodes[index].view.parent = parent;

    for (pugi::xml_node child : node.children()) {
        if (child.type() == pugi::node_element && !gather(xml, child, index, depth + 1)) {
            return false;
        }
    }

    return true;
}

static bool whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool index_source(EsXml &xml) {
    const std::string &s = xml.source;
    size_t at = 0, next = 0;
    std::vector<size_t> stack;

    while ((at = s.find('<', at)) != std::string::npos) {
        size_t skip = std::string::npos;
        size_t skip_size = 0;

        if (s.compare(at, 4, "<!--") == 0) {
            skip = s.find("-->", at + 4);
            skip_size = 3;
        } else if (s.compare(at, 9, "<![CDATA[") == 0) {
            skip = s.find("]]>", at + 9);
            skip_size = 3;
        } else if (s.compare(at, 2, "<?") == 0) {
            skip = s.find("?>", at + 2);
            skip_size = 2;
        } else if (s.compare(at, 2, "<!") == 0) {
            return false;
        }

        if (skip_size) {
            if (skip == std::string::npos) {
                return false;
            }

            at = skip + skip_size;
            continue;
        }

        if (s.compare(at, 2, "</") == 0) {
            size_t end = s.find('>', at + 2);

            if (stack.empty() || end == std::string::npos) {
                return false;
            }

            xml.nodes[stack.back()].view.close = at;
            stack.pop_back();
            at = end + 1;
            continue;
        }

        if (next >= xml.nodes.size()) {
            return false;
        }

        size_t index = next++;
        XmlEntry &entry = xml.nodes[index];
        entry.view.begin = at;
        size_t cursor = at + 1;

        while (cursor < s.size() && !whitespace(s[cursor]) && s[cursor] != '/' &&
               s[cursor] != '>') {
            ++cursor;
        }

        if (s.compare(at + 1, cursor - at - 1, entry.view.name)) {
            return false;
        }

        while (cursor < s.size()) {
            while (cursor < s.size() && whitespace(s[cursor])) {
                ++cursor;
            }

            if (cursor >= s.size() || s[cursor] == '/' || s[cursor] == '>') {
                break;
            }

            size_t key_begin = cursor;

            while (cursor < s.size() && !whitespace(s[cursor]) && s[cursor] != '=') {
                ++cursor;
            }

            std::string key = s.substr(key_begin, cursor - key_begin);

            while (cursor < s.size() && whitespace(s[cursor])) {
                ++cursor;
            }

            if (cursor >= s.size() || s[cursor++] != '=') {
                return false;
            }

            while (cursor < s.size() && whitespace(s[cursor])) {
                ++cursor;
            }

            if (cursor >= s.size() || (s[cursor] != '"' && s[cursor] != '\'')) {
                return false;
            }

            char quote = s[cursor++];
            size_t begin = cursor;
            size_t end = s.find(quote, begin);
            pugi::xml_attribute attribute = entry.node.attribute(key.c_str());

            if (end == std::string::npos || !attribute || entry.attributes.size() >= 128) {
                return false;
            }

            for (const EsXmlAttribute &prior : entry.attributes) {
                if (key == prior.name) {
                    return false;
                }
            }

            entry.attributes.push_back({attribute.name(), attribute.value(), begin, end});
            cursor = end + 1;
        }

        if (cursor >= s.size()) {
            return false;
        }

        entry.view.empty = s[cursor] == '/';
        entry.view.close = cursor;

        if (entry.view.empty && (cursor + 1 >= s.size() || s[++cursor] != '>')) {
            return false;
        }

        if (s[cursor] != '>') {
            return false;
        }

        entry.view.header_end = cursor + 1;

        if (!entry.view.empty) {
            stack.push_back(index);
        }

        at = cursor + 1;
    }

    return stack.empty() && next == xml.nodes.size();
}

EsXml *es_xml_parse(const void *source, size_t size, char *error, size_t cap) {
    EsXml *xml = nullptr;

    try {
        if (!source || size > 8 * 1024 * 1024) {
            es_fail(error, cap, DM_TEXT_REPOSITORY_NATIVE_XML_DEFINITION_EXCEEDS_ITS_SIZE_LIMIT);
            return nullptr;
        }

        xml = new EsXml;
        xml->source.assign(static_cast<const char *>(source), size);
        pugi::xml_parse_result result =
            xml->document.load_buffer(source, size, pugi::parse_full, pugi::encoding_utf8);
        pugi::xml_node root = xml->document.document_element();
        bool ok = result && root && gather(*xml, root, SIZE_MAX, 0);

        for (pugi::xml_node node : xml->document.children()) {
            if (node.type() == pugi::node_doctype ||
                (node.type() == pugi::node_element && node != root)) {
                ok = false;
            }
        }

        if (!ok || !index_source(*xml)) {
            es_fail(
                error, cap,
                DM_TEXT_REPOSITORY_MALFORMED_DUPLICATE_ATTRIBUTE_OR_UNSUPPORTED_NATIVE_XML_DEFINITION);
            delete xml;
            return nullptr;
        }

        for (XmlEntry &entry : xml->nodes) {
            entry.view.attributes = entry.attributes.data();
            entry.view.attribute_count = entry.attributes.size();
        }

        return xml;
    } catch (const std::bad_alloc &) {
        delete xml;
        es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_BOUNDED_NATIVE_XML_DEFINITION);
        return nullptr;
    }
}

size_t es_xml_count(const EsXml *xml) {
    return xml->nodes.size();
}

const EsXmlNode *es_xml_node(const EsXml *xml, size_t index) {
    return index < xml->nodes.size() ? &xml->nodes[index].view : nullptr;
}

const char *es_xml_attribute(const EsXmlNode *node, const char *name) {
    if (node) {
        for (size_t i = 0; i < node->attribute_count; ++i) {
            if (!strcmp(node->attributes[i].name, name)) {
                return node->attributes[i].value;
            }
        }
    }

    return nullptr;
}

char *es_xml_escape(const char *value) {
    try {
        std::string escaped;

        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
            switch (*p) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&apos;";
                break;
            default:
                if (*p < 32 && *p != '\n' && *p != '\r' && *p != '\t') {
                    return nullptr;
                }

                escaped += static_cast<char>(*p);
                break;
            }
        }

        char *out = static_cast<char *>(malloc(escaped.size() + 1));

        if (out) {
            memcpy(out, escaped.c_str(), escaped.size() + 1);
        }

        return out;
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}

bool es_xml_set_attribute(EsXml *xml, size_t index, const char *name, const char *value,
                          EsEdits *edits, char *error, size_t cap) {
    const EsXmlNode *node = es_xml_node(xml, index);
    char *escaped = node && value ? es_xml_escape(value) : nullptr;

    if (!escaped) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ENCODE_NATIVE_XML_ATTRIBUTE);
    }

    bool ok = false;

    for (size_t i = 0; i < node->attribute_count; ++i) {
        const EsXmlAttribute &attribute = node->attributes[i];

        if (!strcmp(attribute.name, name)) {
            ok = es_edits_add(edits, attribute.begin, attribute.end, escaped, strlen(escaped),
                              error, cap);
            free(escaped);
            return ok;
        }
    }

    try {
        std::string insertion = std::string(" ") + name + "=\"" + escaped + "\"";
        size_t at = node->header_end - (node->empty ? 2 : 1);
        ok = es_edits_add(edits, at, at, insertion.c_str(), insertion.size(), error, cap);
    } catch (const std::bad_alloc &) {
        es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_NATIVE_XML_ATTRIBUTE);
    }

    free(escaped);
    return ok;
}

bool es_xml_append(EsXml *xml, size_t index, const char *fragment, EsEdits *edits, char *error,
                   size_t cap) {
    const EsXmlNode *node = es_xml_node(xml, index);

    if (!node || !fragment) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_LOCATE_NATIVE_XML_INSERTION_POINT);
    }

    if (!node->empty) {
        return es_edits_add(edits, node->close, node->close, fragment, strlen(fragment), error,
                            cap);
    }

    try {
        std::string insertion = std::string(">") + fragment + "</" + node->name + ">";
        return es_edits_add(edits, node->header_end - 2, node->header_end, insertion.c_str(),
                            insertion.size(), error, cap);
    } catch (const std::bad_alloc &) {
        return es_fail(error, cap, DM_TEXT_REPOSITORY_CANNOT_ALLOCATE_NATIVE_XML_INSERTION);
    }
}

void es_xml_free(EsXml *xml) {
    delete xml;
}
