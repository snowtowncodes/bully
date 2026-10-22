#pragma once

// Track 30: dialogue system.
//
// The retail game keeps dialogue audio in XACT banks and script data in .lur
// bytecode; both are out of scope for this engine (see the asset catalog).
// Dialogue content is instead hand-authored as JSON (see
// data/dialogue/demo_dialogue.json) and loaded into plain structs — the same
// approach the Track 20 mission system takes.
//
// A dialogue TREE is a map of named NODES. A node carries a speaker, a list
// of text lines, an optional (deferred) voice path, a linear "next" hop, and
// optional choices { text, next }. Playback walks the tree one line at a time
// with a typewriter reveal (0.03 s per character); when every line of the
// node is revealed and the node owns choices, the caller presents them and
// picks one with ChooseOption(). An empty "next" after the last line ends
// the conversation.
//
// The manager is plain data + logic: no SDL, no entities. The Engine drives
// it (input, freeze gates, mission hooks) and the UI draws what the
// accessors expose.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenBully {

struct DialogueChoice {
    std::string text;   // button label
    std::string next;   // node id to jump to ("" = end the dialogue)
};

struct DialogueNode {
    std::string id;
    std::string speaker;
    std::vector<std::string> lines;
    std::string voice;   // optional wav path (audio deferred per asset catalog)
    std::string next;    // linear continuation ("" = end when no choices)
    std::vector<DialogueChoice> choices;
};

struct DialogueTree {
    std::string id;      // tree key ("student", "prefect", ...); NPCs map to it
    std::string root;    // first node id
    std::unordered_map<std::string, DialogueNode> nodes;
};

class DialogueManager {
public:
    // --- loading ---------------------------------------------------------

    // Parses { "trees": [ { "id", "root", "nodes": { id: {...} } } ] }.
    // Returns false (with an error log) when the file is missing/malformed.
    bool LoadFile(const char* path);

    // --- playback ----------------------------------------------------------

    // Starts the tree named `npcId` (Engine maps NPC type/index -> tree id;
    // an unknown id falls back to the first tree) at `nodeId` ("" = root).
    // False when the tree/node does not exist.
    bool StartDialogue(const std::string& npcId, const std::string& nodeId);

    // Typewriter tick: reveals characters of the current line at
    // kCharDelay seconds per character.
    void Update(float dt);

    // E/Space semantics: reveal the full line if it is still typing; else
    // advance to the next line; at the end of a node follow `next` or end.
    // Does nothing while the node's choices are pending (must pick one).
    void Advance();

    // Picks choice `index` (0-based). Jumps to the choice's node; an empty
    // "next" ends the dialogue. False when the choice is invalid.
    bool ChooseOption(int index);

    // Stops playback immediately (fires OnEnd exactly once).
    void EndDialogue();

    bool IsActive() const { return m_active; }

    // Fired exactly once when a conversation ends (Engine: NPC release +
    // MissionManager::CompleteObjective hook).
    void SetOnEnd(std::function<void()> cb) { m_onEnd = std::move(cb); }

    // --- UI accessors ------------------------------------------------------

    // Current line truncated to the revealed character count.
    std::string VisibleText() const;
    // Full text of the current line (for wrapping width calculations).
    const std::string& CurrentLine() const;
    bool LineFullyRevealed() const;
    const std::string& Speaker() const
        { static const std::string empty; return m_node ? m_node->speaker : empty; }
    // Non-null only when the node is fully revealed AND owns choices.
    const std::vector<DialogueChoice>* ActiveChoices() const;
    bool HasVoice() const { return m_node && !m_node->voice.empty(); }

    // --- content access ------------------------------------------------------

    int                        TreeCount() const { return int(m_trees.size()); }
    const DialogueTree*        GetTree(int index) const;
    const std::string&         TreeId(int index) const;
    const DialogueNode*        FindNode(const std::string& treeId,
                                        const std::string& nodeId) const;

    static constexpr float kCharDelay = 0.03f; // seconds per revealed char

private:
    void EnterNode(const std::string& nodeId);

    std::vector<DialogueTree> m_trees;
    const DialogueTree* m_tree  = nullptr;
    const DialogueNode* m_node  = nullptr;
    size_t m_lineIndex   = 0;
    int    m_revealCount = 0;
    float  m_charTimer   = 0.0f;
    bool   m_active      = false;
    std::function<void()> m_onEnd;
};

} // namespace OpenBully
