// Orca Core — deterministic event scheduler.
//
// Time is measured in CPU cycles since power-on. Events fire in order of
// (time, scheduling order), so two runs with the same inputs always produce
// the same sequence. Events may schedule or cancel other events from inside
// their callback; an event scheduled for a time that has already been reached
// fires during the same advance_to() call, after the current one.
#pragma once
#include "orca/core/bits.hpp"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace orca::core {

class Scheduler {
public:
    // Diagnostics: events fired, in total and by name.
    u64 fired() const noexcept { return fired_; }
    const std::unordered_map<std::string, u64>& fired_by_name() const noexcept { return fired_by_name_; }

    using EventId = u64;
    // `now` is the event's due time (time advances event by event); `late` is
    // non-zero only for events that were scheduled for a time already past.
    using Callback = std::function<void(u64 now, u64 late)>;

    u64 now() const noexcept { return now_; }

    // Schedules at an absolute time. Times in the past fire at the next advance.
    EventId schedule_at(u64 time, std::string name, Callback callback);
    EventId schedule_in(u64 delay, std::string name, Callback callback) {
        return schedule_at(now_ + delay, std::move(name), std::move(callback));
    }
    // Returns false if the event already fired or does not exist.
    bool cancel(EventId id);
    bool is_pending(EventId id) const { return index_.count(id) != 0; }

    std::optional<u64> next_event_time() const;
    std::size_t pending() const noexcept { return queue_.size(); }

    // Moves time forward to `time` (never backwards) and fires every event due.
    void advance_to(u64 time);

private:
    struct Event {
        std::string name;
        Callback callback;
    };
    using Key = std::pair<u64, u64>; // (time, sequence)
    std::map<Key, Event> queue_;
    std::unordered_map<EventId, Key> index_;
    u64 now_{0};
    u64 next_seq_{1};
    u64 fired_{0};
    std::unordered_map<std::string, u64> fired_by_name_;
};

} // namespace orca::core
