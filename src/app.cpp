// app.cpp — Item registration helpers and action queue helpers
#include "app.h"

#if !defined(NDEBUG)
thread_local int g_model_lock_depth = 0;
#endif

void item_cache_name_lower(Item* it) {
    if (!it) return;
    it->name_lower = it->name;
    str_lower_inplace(it->name_lower);
}

bool register_item_locked(App& self, ItemKind kind, Item* it) {
    if (!it) return false;
    const int k = ki(kind);

    // Assign uid if caller hasn't.
    if (it->uid == 0) it->uid = self.next_uid++;

    // Cache lowercase name for fast filtering.
    if (it->name_lower.empty() && !it->name.empty())
        item_cache_name_lower(it);

    // Enforce unique name per kind (case-insensitive via by_name hash/eq).
    if (!it->name.empty()) {
        auto f = self.items[k].by_name.find(it->name);
        if (f != self.items[k].by_name.end()) return false;
    }

    // Take ownership and index.
    self.items[k].v.emplace_back(it);
    if (!it->name.empty()) self.items[k].by_name.emplace(it->name, it);
    self.uid_map[it->uid] = it;

    // Clean any stale UI caches for this pointer just in case (defensive).
    self.lv_row_of_item[k].erase(it);

    return true;
}

void unregister_item_locked(App& self, ItemKind kind, Item* it) {
    if (!it) return;
    const int k = ki(kind);

    // uid_map
    if (it->uid) self.uid_map.erase(it->uid);

    // by_name (robust erase by pointer; name may have changed)
    {
        auto& bn = self.items[k].by_name;
        bool erased = false;
        if (!it->name.empty()) {
            auto f = bn.find(it->name);
            if (f != bn.end() && f->second == it) {
                bn.erase(f);
                erased = true;
            }
        }
        if (!erased) {
            for (auto i = bn.begin(); i != bn.end(); ) {
                if (i->second == it) i = bn.erase(i);
                else ++i;
            }
        }
    }

    // dirty_status (remove any queued pointers)
    {
        auto& ds = self.dirty_status[k];
        ds.erase(std::remove(ds.begin(), ds.end(), it), ds.end());
    }

    // UI row cache map
    self.lv_row_of_item[k].erase(it);

    // Remove ownership from vector (linear; sizes are typically modest)
    {
        auto& vec = self.items[k].v;
        for (size_t i = 0; i < vec.size(); i++) {
            if (vec[i].get() == it) {
                vec.erase(vec.begin() + (ptrdiff_t)i);
                break;
            }
        }
    }
}

void unregister_all_items_locked(App& self, ItemKind kind) {
    const int k = ki(kind);
    // Keep popping from the back to avoid O(N^2) from repeated erase at front.
    while (!self.items[k].v.empty()) {
        Item* it = self.items[k].v.back().get();
        unregister_item_locked(self, kind, it);
    }
    // Defensive: clear per-kind caches fully
    self.dirty_status[k].clear();
    self.lv_row_of_item[k].clear();
    self.items[k].by_name.clear();
}

// Action-queue helpers (MUST be called without holding self.mtx)
// These centralize the lock ordering rule: model lock (self.mtx) is never held while
// waiting for / holding the action queue lock (self.threads.action_mtx).
void action_enqueue(App& self, Action&& a) {
    dbg_assert_model_unlocked(__FUNCTION__);

    {
        std::lock_guard<std::mutex> lk(self.threads.action_mtx);
        self.threads.action_q.emplace_back(std::move(a));
    }
    self.threads.action_cv.notify_one();
}

void action_enqueue_batch(App& self, std::vector<Action>&& batch) {
    dbg_assert_model_unlocked(__FUNCTION__);
    if (batch.empty()) return;

    {
        std::lock_guard<std::mutex> lk(self.threads.action_mtx);
        for (auto& a : batch)
            self.threads.action_q.emplace_back(std::move(a));
    }
    self.threads.action_cv.notify_all();
}

size_t action_qdepth(App& self) {
    dbg_assert_model_unlocked(__FUNCTION__);

    std::lock_guard<std::mutex> lk(self.threads.action_mtx);
    return self.threads.action_q.size();
}

void action_clear(App& self) {
    dbg_assert_model_unlocked(__FUNCTION__);

    std::lock_guard<std::mutex> lk(self.threads.action_mtx);
    self.threads.action_q.clear();
}

// Only the action thread should call this.
bool action_pop_wait(std::stop_token st, App& self, Action& out) {
    dbg_assert_model_unlocked(__FUNCTION__);

    std::unique_lock<std::mutex> lk(self.threads.action_mtx);
    self.threads.action_cv.wait(lk, [&] { return st.stop_requested() || !self.threads.action_q.empty(); });
    if (st.stop_requested()) return false;
    out = std::move(self.threads.action_q.front());
    self.threads.action_q.pop_front();
    return true;
}

bool binsearch_nameidx(const NameIdx* arr, size_t n, const wchar_t* key, size_t* out_idx) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = _wcsicmp(arr[mid].name, key);
        if (c == 0) { *out_idx = arr[mid].idx; return true; }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return false;
}
