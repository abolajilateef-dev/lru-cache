// lru_cache.hpp
// LRU cache: std::list (MRU front -> LRU back) + unordered_map (key -> list node), one mutex.

#ifndef LRU_CACHE_HPP
#define LRU_CACHE_HPP

#include <cassert>
#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace lru_test {
class Peer;  // test-only friend, grants access to check_invariants()
}

template <typename K, typename V>
class LruCache {
public:
    // The key lives in the node so eviction can find and rename the matching map entry.
    struct Entry {
        K key;
        V value;
    };

    explicit LruCache(size_t capacity) : capacity_(capacity) {
        assert(capacity_ > 0 && "LruCache capacity must be > 0");
    }

    // Non-copyable and non-movable: the map holds iterators into this object's own list.
    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;
    LruCache(LruCache&&) = delete;
    LruCache& operator=(LruCache&&) = delete;

    // Returns a copy so the value stays valid after the lock is released.
    std::optional<V> get(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;  // miss
        }

        // splice relinks the node without moving it, so stored iterators stay valid.
        list_.splice(list_.begin(), list_, it->second);
        return it->second->value;
    }

    void put(const K& key, V value) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->value = std::move(value);
            list_.splice(list_.begin(), list_, it->second);
            return;
        }

        if (map_.size() < capacity_) {
            list_.emplace_front(Entry{key, std::move(value)});
            map_.emplace(key, list_.begin());
            return;
        }

        // Full: recycle the tail list node and its map node, so eviction allocates nothing.
        auto lru = std::prev(list_.end());  // iterator to the LRU node (tail)
        const K old_key = lru->key;         // copy of the key we are evicting

        lru->key = key;
        lru->value = std::move(value);
        list_.splice(list_.begin(), list_, lru);

        auto node = map_.extract(old_key);  // detaches the map node without freeing it
        assert(!node.empty() && "evicted key must be present in the map");
        node.key() = key;         // mutate the (normally const) stored key
        node.mapped() = lru;
        map_.insert(std::move(node));  // reattach handle, no allocation
    }

    // Locked: reading map_ while another thread modifies it would be a data race.
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return map_.size();
    }

private:
    friend class lru_test::Peer;

    // Debug helper; takes no lock, so call it only from locked code or after threads join.
    void check_invariants() const {
        assert(map_.size() == list_.size());
        assert(list_.size() <= capacity_);
        for (auto it = list_.begin(); it != list_.end(); ++it) {
            auto m = map_.find(it->key);
            assert(m != map_.end() && "list key missing from map");
            assert(m->second == it && "map iterator does not point at this node");
        }
    }

    size_t capacity_;

    // One mutex guards list_ and map_; get() mutates the list, so shared_mutex would be unsafe.
    mutable std::mutex mutex_;
    std::list<Entry> list_;
    std::unordered_map<K, typename std::list<Entry>::iterator> map_;
};

#endif  // LRU_CACHE_HPP
