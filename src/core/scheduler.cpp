#include "orca/core/scheduler.hpp"

namespace orca::core {

Scheduler::EventId Scheduler::schedule_at(u64 time, std::string name, Callback callback) {
    const EventId id = next_seq_++;
    const Key key{time, id};
    queue_.emplace(key, Event{std::move(name), std::move(callback)});
    index_.emplace(id, key);
    return id;
}

bool Scheduler::cancel(EventId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) return false;
    queue_.erase(it->second);
    index_.erase(it);
    return true;
}

std::optional<u64> Scheduler::next_event_time() const {
    if (queue_.empty()) return std::nullopt;
    return queue_.begin()->first.first;
}

void Scheduler::advance_to(u64 time) {
    if (time > now_) now_ = time;
    while (!queue_.empty() && queue_.begin()->first.first <= now_) {
        auto node = queue_.extract(queue_.begin());
        index_.erase(node.key().second);
        const u64 due = node.key().first;
        node.mapped().callback(now_, now_ - due);
    }
}

} // namespace orca::core
