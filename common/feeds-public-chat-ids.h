#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>

// The ids of the public chat messages an engine has forwarded to the plugin.
//
// The SDK's delete callback carries only a message id, with no way to tell
// whether the message was public. This set is how an engine keeps the privacy
// filter at its own boundary: a delete goes to the plugin only for an id that
// already went there as a public message, so nothing about a private message
// crosses the IPC.
//
// Bounded, oldest out. The cap only has to outlast the plugin's own retention
// (500 dock rows, 50 overlay messages); a delete for an id older than that has
// nothing left to remove. Ids are unique across meetings, so it is never cleared.
namespace feeds {

class PublicChatIds {
public:
    void Add(const std::string& id) {
        if (id.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_ids.insert(id).second) return;
        m_order.push_back(id);
        if (m_order.size() > kCap) {
            m_ids.erase(m_order.front());
            m_order.pop_front();
        }
    }

    bool Contains(const std::string& id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ids.count(id) != 0;
    }

private:
    static constexpr std::size_t kCap = 1000;

    std::mutex                      m_mutex;
    std::unordered_set<std::string> m_ids;
    std::deque<std::string>         m_order;
};

}  // namespace feeds
