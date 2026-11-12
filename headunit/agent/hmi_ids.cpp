// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The ids of a PluginApp: see hmi_ids.hpp.

#include "hmi_ids.hpp"

namespace nbt {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
}

// A whole number with nothing but spaces around it.
bool number(const std::string& text, long* out) {
    size_t begin = 0, end = text.size();
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    if (begin == end || end - begin > 9) return false;
    long value = 0;
    for (size_t i = begin; i < end; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + (text[i] - '0');
    }
    *out = value;
    return true;
}

std::string trimmed(const std::string& text) {
    size_t begin = 0, end = text.size();
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

class Handler {
public:
    virtual ~Handler() {}
    // An element opens: its name, its parent's name, and its attributes as name, value, name, value...
    virtual void element(const std::string& name, const std::string& parent, const std::vector<std::string>& attributes) = 0;
    // The text directly inside an element that has just closed.
    virtual void text(const std::string& name, const std::string& parent, const std::string& text) = 0;
};

void scan(const std::string& xml, Handler* handler) {
    std::vector<std::string> open;    // the elements we are inside
    std::vector<std::string> texts;   // the text of each so far
    size_t at = 0;
    while (at < xml.size()) {
        if (xml[at] != '<') {
            size_t next = xml.find('<', at);
            if (next == std::string::npos) next = xml.size();
            if (!texts.empty()) texts.back().append(xml, at, next - at);
            at = next;
            continue;
        }
        if (xml.compare(at, 4, "<!--") == 0) {
            size_t end = xml.find("-->", at + 4);
            if (end == std::string::npos) return;
            at = end + 3;
        } else if (xml.compare(at, 9, "<![CDATA[") == 0) {
            size_t end = xml.find("]]>", at + 9);
            if (end == std::string::npos) return;
            if (!texts.empty()) texts.back().append(xml, at + 9, end - at - 9);
            at = end + 3;
        } else if (xml.compare(at, 2, "<?") == 0 || xml.compare(at, 2, "<!") == 0) {
            size_t end = xml.find('>', at);
            if (end == std::string::npos) return;
            at = end + 1;
        } else if (xml.compare(at, 2, "</") == 0) {
            size_t end = xml.find('>', at);
            if (end == std::string::npos) return;
            std::string name = trimmed(xml.substr(at + 2, end - at - 2));
            at = end + 1;
            // Close up to the matching element; a stray end tag is skipped.
            size_t match = open.size();
            while (match > 0 && open[match - 1] != name) --match;
            if (match == 0) continue;
            while (open.size() >= match) {
                std::string parent = open.size() > 1 ? open[open.size() - 2] : std::string();
                handler->text(open.back(), parent, texts.back());
                open.pop_back();
                texts.pop_back();
            }
        } else {
            size_t p = at + 1;
            size_t nameStart = p;
            while (p < xml.size() && isNameChar(xml[p])) ++p;
            std::string name = xml.substr(nameStart, p - nameStart);
            if (name.empty()) {
                ++at;   // a bare '<': not markup we know
                continue;
            }
            std::vector<std::string> attributes;
            bool selfClosing = false;
            for (;;) {
                while (p < xml.size() && isSpace(xml[p])) ++p;
                if (p >= xml.size()) return;
                if (xml[p] == '>') {
                    ++p;
                    break;
                }
                if (xml[p] == '/') {
                    selfClosing = true;
                    ++p;
                    continue;
                }
                size_t attrStart = p;
                while (p < xml.size() && isNameChar(xml[p])) ++p;
                if (p == attrStart) {
                    ++p;   // something unexpected inside the tag: step over it
                    continue;
                }
                std::string attr = xml.substr(attrStart, p - attrStart);
                while (p < xml.size() && isSpace(xml[p])) ++p;
                if (p >= xml.size() || xml[p] != '=') continue;   // an attribute with no value
                ++p;
                while (p < xml.size() && isSpace(xml[p])) ++p;
                if (p >= xml.size() || (xml[p] != '"' && xml[p] != '\'')) continue;
                char quote = xml[p++];
                size_t valueEnd = xml.find(quote, p);
                if (valueEnd == std::string::npos) return;
                attributes.push_back(attr);
                attributes.push_back(xml.substr(p, valueEnd - p));
                p = valueEnd + 1;
            }
            at = p;
            std::string parent = open.empty() ? std::string() : open.back();
            handler->element(name, parent, attributes);
            if (selfClosing) {
                handler->text(name, parent, std::string());
            } else {
                open.push_back(name);
                texts.push_back(std::string());
            }
        }
    }
}

class ManifestHandler : public Handler {
public:
    ManifestHandler(std::vector<long>* ids, std::string* descFile) : ids_(ids), descFile_(descFile) {}
    virtual void element(const std::string&, const std::string&, const std::vector<std::string>&) {}
    virtual void text(const std::string& name, const std::string& parent, const std::string& text) {
        long id = 0;
        if (name == "id" && parent == "pluginAppConnector" && number(text, &id)) ids_->push_back(id);
        if (name == "DescFile" && descFile_->empty()) *descFile_ = trimmed(text);
    }

private:
    std::vector<long>* ids_;
    std::string* descFile_;
};

class DescriptionHandler : public Handler {
public:
    explicit DescriptionHandler(std::vector<long>* ids) : ids_(ids) {}
    virtual void element(const std::string& name, const std::string&, const std::vector<std::string>& attributes) {
        if (name != "pluginApp") return;
        for (size_t i = 0; i + 1 < attributes.size(); i += 2) {
            long id = 0;
            if (attributes[i] == "entryButtonFocusID" && number(attributes[i + 1], &id)) ids_->push_back(id);
        }
    }
    virtual void text(const std::string&, const std::string&, const std::string&) {}

private:
    std::vector<long>* ids_;
};

}  // namespace

void readPluginManifest(const std::string& xml, std::vector<long>* connectorIds, std::string* descFile) {
    descFile->clear();
    ManifestHandler handler(connectorIds, descFile);
    scan(xml, &handler);
}

void readPluginDescription(const std::string& xml, std::vector<long>* focusIds) {
    DescriptionHandler handler(focusIds);
    scan(xml, &handler);
}

bool builtInConnectorId(long id) { return id == 17 || id == 200; }

}  // namespace nbt
