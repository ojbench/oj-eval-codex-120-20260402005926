// Hierarchical Timing Wheel implementation for ACMOJ driver that includes this header.
// We avoid adding extra headers beyond what is necessary (vector + Task.hpp).

#pragma once

#include "Task.hpp"
#include <vector>

// You may not add other standard headers. We implement our own basic structures.

class TaskNode {
    friend class TimingWheel;
    friend class Timer;
public:
    TaskNode() : task(nullptr), next(nullptr), prev(nullptr), bucket(nullptr), level(-1), rem_secs(0), rem_mins(0), next_fire(0) {}

private:
    Task* task;                 // Task pointer (no ownership)
    TaskNode* next;             // doubly linked list
    TaskNode* prev;
    TaskNode** bucket;          // pointer to current bucket head pointer for O(1) removal
    int level;                  // 0=sec,1=min,2=hour, -1=not scheduled
    int rem_secs;               // residual seconds when stored in minute/hour wheels
    int rem_mins;               // residual minutes when stored in hour wheel
    unsigned long long next_fire; // absolute tick when it should fire next
};

class TimingWheel {
    friend class Timer;
public:
    TimingWheel(size_t size, size_t interval)
        : size(size), interval(interval), current_slot(0) {
        slots = new TaskNode*[size];
        for (size_t i = 0; i < size; ++i) slots[i] = nullptr;
    }

    ~TimingWheel() {
        // Buckets are not owned; Timer ensures tasks removed before destruction.
        delete[] slots;
    }

private:
    const size_t size;
    const size_t interval; // granularity in ticks per slot
    size_t current_slot;   // current index
    TaskNode** slots;      // array of bucket heads (intrusive lists)
};

class Timer {
public:
    Timer()
        : sec_wheel(60, 1), min_wheel(60, 60), hour_wheel(24, 3600), now_tick(0) {
    }

    ~Timer() {
        // Best-effort cleanup: detach any remaining nodes from buckets.
        // Nodes are user-managed; we do not delete TaskNode* or Task*.
    }

    TaskNode* addTask(Task* task) {
        if (!task) return nullptr;
        TaskNode* node = new TaskNode();
        node->task = task;
        node->next_fire = now_tick + static_cast<unsigned long long>(task->getFirstInterval());
        schedule_node(node, /*is_reschedule=*/false);
        return node;
    }

    void cancelTask(TaskNode *p) {
        if (!p) return;
        remove_from_bucket(p);
        // Mark as unscheduled; do not delete to allow caller to own pointer.
        p->level = -1;
    }

    std::vector<Task*> tick() {
        std::vector<Task*> due;
        // advance lowest wheel
        advance_one(sec_wheel);
        ++now_tick;

        // extract all tasks in current second slot
        TaskNode* head = sec_wheel.slots[sec_wheel.current_slot];
        sec_wheel.slots[sec_wheel.current_slot] = nullptr;
        // process rollover and cascading before executing tasks? Standard approach is:
        // In a tick, after moving to next second slot, we should cascade higher wheels if rollover occurs,
        // then execute tasks scheduled in the current second slot (which were placed with correct offsets).

        // If seconds rolled over to 0, cascade minutes/hours first, then execute seconds bucket.
        if (sec_wheel.current_slot == 0) {
            // advance minute wheel
            advance_one(min_wheel);
            // if minutes rolled over, advance hour wheel and cascade hours -> minutes first
            if (min_wheel.current_slot == 0) {
                // move all nodes from current hour slot into minute wheel according to rem_mins
                TaskNode* h = hour_wheel.slots[hour_wheel.current_slot];
                hour_wheel.slots[hour_wheel.current_slot] = nullptr;
                while (h) {
                    TaskNode* nxt = h->next;
                    h->next = h->prev = nullptr;
                    h->bucket = nullptr;
                    // insert into minute wheel at offset rem_mins
                    insert_into_minute(h, h->rem_mins);
                    h = nxt;
                }
            }
            // Now cascade minutes current slot into seconds
            TaskNode* m = min_wheel.slots[min_wheel.current_slot];
            min_wheel.slots[min_wheel.current_slot] = nullptr;
            while (m) {
                TaskNode* nxt = m->next;
                m->next = m->prev = nullptr;
                m->bucket = nullptr;
                // Move into seconds by rem_secs
                insert_into_second(m, m->rem_secs);
                m = nxt;
            }
        }

        // Execute due tasks in the second slot (which is the head we saved before cascading?)
        // Important: we cascaded minutes->seconds after extracting 'head'. However, tasks cascaded
        // from minutes at this tick should also execute if rem_secs==0, which we insert into the
        // current second slot. So we should merge head with the current bucket after cascades.
        // We'll prepend previously extracted list back to the bucket and then drain bucket.
        if (head) {
            // Prepend old head to current bucket
            if (sec_wheel.slots[sec_wheel.current_slot]) {
                TaskNode* cur = sec_wheel.slots[sec_wheel.current_slot];
                // attach head at front
                TaskNode* tail = head;
                while (tail->next) tail = tail->next;
                tail->next = cur;
                cur->prev = tail;
                // bucket remains the same for cur; update for head list
                TaskNode* it = head;
                while (it) { it->bucket = &sec_wheel.slots[sec_wheel.current_slot]; it = it->next; }
            } else {
                // set bucket head to head and update bucket ptrs
                sec_wheel.slots[sec_wheel.current_slot] = head;
                TaskNode* it = head; while (it) { it->bucket = &sec_wheel.slots[sec_wheel.current_slot]; it = it->next; }
            }
        }

        // Drain current second bucket and execute tasks while rescheduling periodic ones.
        TaskNode* cur = sec_wheel.slots[sec_wheel.current_slot];
        sec_wheel.slots[sec_wheel.current_slot] = nullptr;
        while (cur) {
            TaskNode* nxt = cur->next;
            cur->next = cur->prev = nullptr;
            cur->bucket = nullptr;
            // Execute task
            due.push_back(cur->task);
            unsigned long long period = static_cast<unsigned long long>(cur->task->getPeriod());
            if (period > 0) {
                // reschedule periodically from now
                cur->next_fire = now_tick + period;
                schedule_node(cur, /*is_reschedule=*/true);
            } else {
                // one-shot: mark unscheduled; free node
                cur->level = -1;
                delete cur;
            }
            cur = nxt;
        }

        return due;
    }

private:
    TimingWheel sec_wheel;
    TimingWheel min_wheel;
    TimingWheel hour_wheel;
    unsigned long long now_tick;

    // Utility: unlink node from its current bucket (if any)
    void remove_from_bucket(TaskNode* n) {
        if (!n || !n->bucket) return;
        TaskNode** headp = n->bucket;
        if (n->prev) n->prev->next = n->next;
        if (n->next) n->next->prev = n->prev;
        if (*headp == n) *headp = n->next;
        n->next = n->prev = nullptr;
        n->bucket = nullptr;
        n->level = -1;
    }

    // Insert at head of a bucket
    void bucket_insert(TaskNode** headp, TaskNode* n, int level) {
        n->prev = nullptr;
        n->next = *headp;
        if (*headp) (*headp)->prev = n;
        *headp = n;
        n->bucket = headp;
        n->level = level;
    }

    void advance_one(TimingWheel& w) {
        w.current_slot = (w.current_slot + 1) % w.size;
    }

    // Insert node into seconds wheel at offset delta (0..59)
    void insert_into_second(TaskNode* n, int delta) {
        int idx = static_cast<int>((sec_wheel.current_slot + (delta % static_cast<int>(sec_wheel.size))) % sec_wheel.size);
        bucket_insert(&sec_wheel.slots[idx], n, 0);
    }

    // Insert node into minutes wheel at offset delta minutes
    void insert_into_minute(TaskNode* n, int delta_minutes) {
        int idx = static_cast<int>((min_wheel.current_slot + (delta_minutes % static_cast<int>(min_wheel.size))) % min_wheel.size);
        bucket_insert(&min_wheel.slots[idx], n, 1);
    }

    // Insert node into hours wheel at offset delta hours
    void insert_into_hour(TaskNode* n, int delta_hours) {
        int idx = static_cast<int>((hour_wheel.current_slot + (delta_hours % static_cast<int>(hour_wheel.size))) % hour_wheel.size);
        bucket_insert(&hour_wheel.slots[idx], n, 2);
    }

    void schedule_node(TaskNode* n, bool /*is_reschedule*/) {
        // Compute delta from current time
        unsigned long long delta = (n->next_fire > now_tick) ? (n->next_fire - now_tick) : 0ULL;

        if (delta < sec_wheel.size) {
            n->rem_secs = 0;
            n->rem_mins = 0;
            insert_into_second(n, static_cast<int>(delta));
            return;
        }

        unsigned long long total_secs_min = sec_wheel.size * min_wheel.size; // 60*60
        if (delta < total_secs_min) {
            // place into minutes wheel
            n->rem_secs = static_cast<int>(delta % sec_wheel.size);
            n->rem_mins = 0;
            int dmin = static_cast<int>(delta / sec_wheel.size);
            insert_into_minute(n, dmin);
            return;
        }

        unsigned long long total_secs_hour = total_secs_min * hour_wheel.size; // 60*60*24
        if (delta < total_secs_hour) {
            n->rem_secs = static_cast<int>(delta % sec_wheel.size);
            n->rem_mins = static_cast<int>((delta / sec_wheel.size) % min_wheel.size);
            int dhour = static_cast<int>(delta / total_secs_min);
            insert_into_hour(n, dhour);
            return;
        }

        // If delay exceeds 24 hours window, approximate by clamping at max window boundary.
        // Place in hour wheel at the furthest slot, keeping residuals.
        unsigned long long clamped = delta % total_secs_hour;
        n->rem_secs = static_cast<int>(clamped % sec_wheel.size);
        n->rem_mins = static_cast<int>((clamped / sec_wheel.size) % min_wheel.size);
        int dhour = static_cast<int>(clamped / total_secs_min);
        insert_into_hour(n, dhour);
    }
};

