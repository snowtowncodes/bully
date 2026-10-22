#include "dialogue.h"

#include "../platform/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace OpenBully {

// --- Minimal JSON value parser (same model as mission.cpp: the project
// vendors no JSON library; this is a private copy for dialogue content) -----

namespace {

struct JValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JValue> items;                           // Array
    std::vector<std::pair<std::string, JValue>> members; // Object

    const JValue* Find(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : members)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : m_text(text) {}

    bool Parse(JValue& out, std::string& err) {
        SkipWs();
        if (!ParseValue(out)) { err = Error("value"); return false; }
        SkipWs();
        if (m_pos != m_text.size()) { err = Error("trailing characters"); return false; }
        return true;
    }

private:
    const std::string& m_text;
    size_t m_pos = 0;
    int m_line = 1;

    void SkipWs() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == '\n') { ++m_line; ++m_pos; }
            else if (c == ' ' || c == '\t' || c == '\r') ++m_pos;
            else break;
        }
    }
    std::string Error(const char* what) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "line %d (byte %zu): expected %s near \"%.24s\"",
                      m_line, m_pos, what, m_text.c_str() + std::min(m_pos, m_text.size()));
        return buf;
    }
    bool Consume(char c) {
        SkipWs();
        if (m_pos < m_text.size() && m_text[m_pos] == c) { ++m_pos; return true; }
        return false;
    }
    bool Peek(char c) {
        SkipWs();
        return m_pos < m_text.size() && m_text[m_pos] == c;
    }

    bool ParseValue(JValue& v) {
        SkipWs();
        if (m_pos >= m_text.size()) return false;
        const char c = m_text[m_pos];
        switch (c) {
            case '{':  return ParseObject(v);
            case '[':  return ParseArray(v);
            case '"':  v.type = JValue::Type::String; return ParseString(v.str);
            case 't':  return ParseLit("true", v);
            case 'f':  return ParseLit("false", v);
            case 'n':  return ParseLit("null", v);
            default:   return ParseNumber(v);
        }
    }
    bool ParseLit(const char* lit, JValue& v) {
        const size_t n = std::strlen(lit);
        if (m_text.compare(m_pos, n, lit) != 0) return false;
        m_pos += n;
        if (lit[0] == 't') { v.type = JValue::Type::Bool; v.boolean = true; }
        else if (lit[0] == 'f') { v.type = JValue::Type::Bool; v.boolean = false; }
        else v.type = JValue::Type::Null;
        return true;
    }
    bool ParseNumber(JValue& v) {
        const char* begin = m_text.c_str() + m_pos;
        char* end = nullptr;
        const double d = std::strtod(begin, &end);
        if (end == begin) return false;
        m_pos += size_t(end - begin);
        v.type = JValue::Type::Number;
        v.number = d;
        return true;
    }
    bool ParseString(std::string& out) {
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') return false;
        ++m_pos;
        out.clear();
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') return true;
            if (c == '\\' && m_pos < m_text.size()) {
                const char e = m_text[m_pos++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    default:   out += e;    break;
                }
            } else {
                out += c;
            }
        }
        return false; // unterminated
    }
    bool ParseArray(JValue& v) {
        v.type = JValue::Type::Array;
        ++m_pos; // [
        if (Peek(']')) { ++m_pos; return true; }
        while (true) {
            v.items.emplace_back();
            if (!ParseValue(v.items.back())) return false;
            if (Consume(',')) continue;
            if (Consume(']')) return true;
            return false;
        }
    }
    bool ParseObject(JValue& v) {
        v.type = JValue::Type::Object;
        ++m_pos; // {
        if (Peek('}')) { ++m_pos; return true; }
        while (true) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            if (!Consume(':')) return false;
            v.members.emplace_back(std::move(key), JValue{});
            if (!ParseValue(v.members.back().second)) return false;
            if (Consume(',')) continue;
            if (Consume('}')) return true;
            return false;
        }
    }
};

void ParseNode(const JValue& jo, DialogueNode& node) {
    if (const JValue* s = jo.Find("id"); s) node.id = s->str;
    if (const JValue* s = jo.Find("speaker"); s) node.speaker = s->str;
    if (const JValue* s = jo.Find("voice"); s) node.voice = s->str;
    if (const JValue* s = jo.Find("next"); s) node.next = s->str;
    if (const JValue* arr = jo.Find("lines"); arr && arr->type == JValue::Type::Array)
        for (const JValue& l : arr->items)
            if (l.type == JValue::Type::String) node.lines.push_back(l.str);
    if (const JValue* arr = jo.Find("choices"); arr && arr->type == JValue::Type::Array) {
        for (const JValue& jc : arr->items) {
            DialogueChoice c;
            if (const JValue* s = jc.Find("text"); s) c.text = s->str;
            if (const JValue* s = jc.Find("next"); s) c.next = s->str;
            node.choices.push_back(std::move(c));
        }
    }
}

} // namespace

// --- DialogueManager -----------------------------------------------------------

bool DialogueManager::LoadFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Logger::Warning("Dialogue: cannot open %s (no dialogue loaded)", path);
        return false;
    }
    const std::string raw((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    // Tolerate a UTF-8 BOM (common from PowerShell-written files).
    const std::string text =
        (raw.size() >= 3 && uint8_t(raw[0]) == 0xEF && uint8_t(raw[1]) == 0xBB &&
         uint8_t(raw[2]) == 0xBF)
            ? raw.substr(3) : raw;

    JValue root;
    std::string err;
    if (!JsonParser(text).Parse(root, err)) {
        Logger::Error("Dialogue: JSON parse failed in %s (%s)", path, err.c_str());
        return false;
    }
    const JValue* trees = root.Find("trees");
    if (!trees || trees->type != JValue::Type::Array) {
        Logger::Error("Dialogue: %s has no \"trees\" array", path);
        return false;
    }

    size_t nodeCount = 0;
    for (const JValue& jt : trees->items) {
        DialogueTree tree;
        if (const JValue* s = jt.Find("id"); s) tree.id = s->str;
        if (const JValue* s = jt.Find("root"); s) tree.root = s->str;
        if (const JValue* nodes = jt.Find("nodes");
            nodes && nodes->type == JValue::Type::Object) {
            for (const auto& [id, jn] : nodes->members) {
                DialogueNode node;
                ParseNode(jn, node);
                if (node.id.empty()) node.id = id;
                if (node.id.empty()) continue;
                tree.nodes.emplace(node.id, std::move(node));
                ++nodeCount;
            }
        }
        if (tree.id.empty() || tree.root.empty() || tree.nodes.empty()) {
            Logger::Warning("Dialogue: skipping tree with no id/root/nodes");
            continue;
        }
        Logger::Info("Dialogue: tree '%s' registered (%zu node(s), root '%s')",
                     tree.id.c_str(), tree.nodes.size(), tree.root.c_str());
        m_trees.push_back(std::move(tree));
    }

    Logger::Info("Dialogue: loaded %zu tree(s), %zu node(s) from %s",
                 m_trees.size(), nodeCount, path);
    return !m_trees.empty();
}

bool DialogueManager::StartDialogue(const std::string& npcId,
                                    const std::string& nodeId) {
    if (m_trees.empty()) return false;

    // Resolve the tree: exact id match first, else the first tree.
    m_tree = nullptr;
    for (const DialogueTree& t : m_trees)
        if (t.id == npcId) { m_tree = &t; break; }
    if (!m_tree) m_tree = &m_trees.front();

    const std::string start = nodeId.empty() ? m_tree->root : nodeId;
    if (m_tree->nodes.find(start) == m_tree->nodes.end()) {
        Logger::Warning("Dialogue: tree '%s' has no node '%s'",
                        m_tree->id.c_str(), start.c_str());
        return false;
    }

    m_active = true;
    EnterNode(start);
    Logger::Info("Dialogue: started tree '%s' at '%s'",
                 m_tree->id.c_str(), start.c_str());
    return true;
}

void DialogueManager::EnterNode(const std::string& nodeId) {
    m_node = nullptr;
    m_lineIndex = 0;
    m_revealCount = 0;
    m_charTimer = 0.0f;
    if (!m_tree) { EndDialogue(); return; }
    auto it = m_tree->nodes.find(nodeId);
    if (it == m_tree->nodes.end()) {
        Logger::Warning("Dialogue: node '%s' missing (tree '%s') — ending",
                        nodeId.c_str(), m_tree->id.c_str());
        EndDialogue();
        return;
    }
    m_node = &it->second;
    // A prompt node with no lines (choices only) is immediately answered.
    if (m_node->lines.empty() && !m_node->choices.empty() && !HasVoice())
        return; // reveal state stays "done"; ActiveChoices() shows the menu
}

void DialogueManager::Update(float dt) {
    if (!m_active || !m_node) return;
    if (m_lineIndex >= m_node->lines.size()) return;
    const std::string& line = m_node->lines[m_lineIndex];
    const int total = int(line.size());
    if (m_revealCount >= total) return;
    m_charTimer += dt;
    while (m_charTimer >= kCharDelay && m_revealCount < total) {
        m_charTimer -= kCharDelay;
        ++m_revealCount;
    }
}

void DialogueManager::Advance() {
    if (!m_active || !m_node) return;
    // Still typing: snap the reveal to the full line.
    if (m_lineIndex < m_node->lines.size() &&
        m_revealCount < int(m_node->lines[m_lineIndex].size())) {
        m_revealCount = int(m_node->lines[m_lineIndex].size());
        return;
    }
    // Choice menu pending: E/Space must not skip past it.
    if (!m_node->choices.empty()) return;

    if (m_lineIndex + 1 < m_node->lines.size()) {
        ++m_lineIndex;
        m_revealCount = 0;
        m_charTimer = 0.0f;
        return;
    }
    // Node finished: follow `next` or end.
    if (!m_node->next.empty()) EnterNode(m_node->next);
    else EndDialogue();
}

bool DialogueManager::ChooseOption(int index) {
    if (!m_active || !m_node) return false;
    if (m_lineIndex < m_node->lines.size() &&
        m_revealCount < int(m_node->lines[m_lineIndex].size()))
        return false; // line still typing
    if (index < 0 || index >= int(m_node->choices.size())) return false;
    const std::string next = m_node->choices[index].next;
    Logger::Info("Dialogue: chose '%s'", m_node->choices[index].text.c_str());
    if (next.empty()) { EndDialogue(); return true; }
    EnterNode(next);
    return true;
}

void DialogueManager::EndDialogue() {
    if (!m_active) return;
    m_active = false;
    m_tree = nullptr;
    m_node = nullptr;
    m_lineIndex = 0;
    m_revealCount = 0;
    if (m_onEnd) {
        auto cb = m_onEnd; // copy: the callback may mutate/destroy us
        cb();
    }
}

std::string DialogueManager::VisibleText() const {
    if (!m_node || m_lineIndex >= m_node->lines.size()) return {};
    const std::string& line = m_node->lines[m_lineIndex];
    const int n = std::min<int>(m_revealCount, int(line.size()));
    return line.substr(0, size_t(n));
}

const std::string& DialogueManager::CurrentLine() const {
    static const std::string empty;
    if (!m_node || m_lineIndex >= m_node->lines.size()) return empty;
    return m_node->lines[m_lineIndex];
}

bool DialogueManager::LineFullyRevealed() const {
    if (!m_node || m_lineIndex >= m_node->lines.size()) return true;
    return m_revealCount >= int(m_node->lines[m_lineIndex].size());
}

const std::vector<DialogueChoice>* DialogueManager::ActiveChoices() const {
    if (!m_active || !m_node || m_node->choices.empty()) return nullptr;
    if (m_lineIndex < m_node->lines.size() && !LineFullyRevealed()) return nullptr;
    return &m_node->choices;
}

const DialogueTree* DialogueManager::GetTree(int index) const {
    return index >= 0 && index < int(m_trees.size()) ? &m_trees[index] : nullptr;
}

const std::string& DialogueManager::TreeId(int index) const {
    static const std::string empty;
    const DialogueTree* t = GetTree(index);
    return t ? t->id : empty;
}

const DialogueNode* DialogueManager::FindNode(const std::string& treeId,
                                              const std::string& nodeId) const {
    for (const DialogueTree& t : m_trees) {
        if (!treeId.empty() && t.id != treeId) continue;
        auto it = t.nodes.find(nodeId);
        if (it != t.nodes.end()) return &it->second;
        if (!treeId.empty()) return nullptr;
    }
    return nullptr;
}

} // namespace OpenBully
