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
    const u64 target = time > now_ ? time : now_;
    // Events fire in order; time is moved to each event's due time before its
    // callback runs, so devices that compute relative timings see the exact
    // instant. `late` is non-zero only for events scheduled in the past.
    while (!queue_.empty() && queue_.begin()->first.first <= target) {
        auto node = queue_.extract(queue_.begin());
        index_.erase(node.key().second);
        const u64 due = node.key().first;
        if (due > now_) now_ = due;
        ++fired_;
        ++fired_by_name_[node.mapped().name];
        node.mapped().callback(now_, now_ - due);
    }
    now_ = target;
}

} // namespace orca::core
