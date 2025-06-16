#pragma once

#include <cstdlib>      // malloc, free
#include <iostream>
#include <vector>
#include <cstdint>
#include <unordered_map>

#if !defined(HASHLL_H)
#define HASHLL_H

namespace HASHLL
{

// ---------------------------------------------------------------------------
// LRU list of virtual pages, with O(1) lookup via an unordered_map.
// Each node represents exactly one virtual page number (vp_num).
// ---------------------------------------------------------------------------
class HashLL
{
public:
    // -----------------------------------------------------------------------
    // node definition
    // -----------------------------------------------------------------------
    struct hash_node
    {
        uint64_t  vp_num;         // virtual‐page number (addr >> 12)
        uint64_t  access_count;   // number of times this page was accessed
        hash_node *next;          // newer (MRU) in the LRU list
        hash_node *prev;          // older (LRU) in the LRU list

        // Constructor now takes full virtual address, not vp_num:
        hash_node(uint64_t vp_addr)
            : vp_num(addr_to_num(vp_addr)), access_count(1),
              next(nullptr), prev(nullptr) {}
    };

    // -----------------------------------------------------------------------
    // Construct an LRU list that can hold up to `capacity` distinct pages.
    // -----------------------------------------------------------------------
    explicit HashLL(uint32_t capacity)
        : cap(capacity), size(0), head(nullptr), tail(nullptr)
    {
        table.reserve(capacity * 2);
    }

    ~HashLL()
    {
        // Delete all nodes in the LRU list
        hash_node *cur = head;
        while (cur)
        {
            hash_node *nxt = cur->next;
            delete cur;
            cur = nxt;
        }
        // The unordered_map destructor will clean itself up automatically
    }

    // -----------------------------------------------------------------------
    // Access (or insert) a page given its virtual address (vp_addr).
    // If the page already exists, bump its access_count and move it to MRU.
    // Otherwise, create a new node at MRU. If over capacity, evict LRU.
    // -----------------------------------------------------------------------
    void touch(uint64_t vp_addr)
    {
        uint64_t vp_num = addr_to_num(vp_addr);
        auto it = table.find(vp_num);
        if (it != table.end())
        {
            // Node already exists: increment and promote to MRU
            hash_node *n = it->second;
            ++n->access_count;
            if (n != head)
            {
                unlink_node(n);
                insert_at_head(n);
            }
        }
        else
        {
            // New page: create node using full address
            hash_node *n = new hash_node(vp_addr);
            table[vp_num] = n;
            insert_at_head(n);
            if (size < cap)
            {
                ++size;
            }
            else
            {
                // Evict LRU (tail)
                hash_node *ev = tail;
                unlink_node(ev);
                table.erase(ev->vp_num);
                delete ev;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Returns useful statistics
    // -----------------------------------------------------------------------
    size_t get_size() const { return size; }
    size_t get_cap()  const { return cap;  }

    // -----------------------------------------------------------------------
    // Make a node MRU, moving it to the front of the list.
    // Also +1 to access count.
    // -----------------------------------------------------------------------
    void make_recent(uint64_t vp_addr)
    {
        uint64_t vp_num = addr_to_num(vp_addr);
        auto it = table.find(vp_num);
        if (it == table.end()) {
            std::cerr << "make_recent: vp_addr " << vp_addr
                      << " (vp_num=" << vp_num << ") not found\n";
            return;
        }

        hash_node *n = it->second;
        ++n->access_count;               // bump the access count

        if (n == head)                   // already MRU?
            return;

        // unlink from current position...
        if (n->prev)      n->prev->next = n->next; else head = n->next;
        if (n->next)      n->next->prev = n->prev; else tail = n->prev;

        // ...and insert at the head
        n->prev = nullptr;
        n->next = head;
        if (head) head->prev = n;
        head = n;
        if (!tail) tail = n;
    }

    // -----------------------------------------------------------------------
    // Return the node with the highest access_count (hottest page),
    // or nullptr if the list is empty.
    // -----------------------------------------------------------------------
    hash_node* hottest_node() const
    {
        hash_node* best = nullptr;
        uint64_t   maxc = 0;
        for (hash_node* cur = head; cur; cur = cur->next) {
            if (cur->access_count > maxc) {
                maxc = cur->access_count;
                best = cur;
            }
        }
        return best;
    }

    // -----------------------------------------------------------------------
    // Return the least‐recently used node (tail), or nullptr if empty.
    // -----------------------------------------------------------------------
    hash_node* lru_node() const
    {
        return tail;
    }

    // -----------------------------------------------------------------------
    // Insert a new page at the LRU position (tail). If already present,
    // does nothing. Does not evict (caller must manage capacity).
    // -----------------------------------------------------------------------
    void insert_lru(uint64_t vp_addr)
    {
        uint64_t vp_num = addr_to_num(vp_addr);
        if (table.count(vp_num)) return;      // already in list
        hash_node* n = new hash_node(vp_addr);
        table[vp_num] = n;
        // append at tail
        n->next = nullptr;
        n->prev = tail;
        if (tail) tail->next = n; else head = n;
        tail = n;
        ++size;
    }

    // -----------------------------------------------------------------------
    // Remove a page from the structure (given its virtual address).
    // If not found, prints a warning.
    // -----------------------------------------------------------------------
    void remove(uint64_t vp_addr)
    {
        uint64_t vp_num = addr_to_num(vp_addr);
        auto it = table.find(vp_num);
        if (it == table.end())
        {
            std::cerr << "remove: vp_addr " << vp_addr
                      << " (vp_num=" << vp_num << ") not found\n";
            return;
        }
        hash_node *n = it->second;
        unlink_node(n);
        table.erase(it);
        delete n;
        --size;
    }

    void increment_count(uint64_t vp_addr)
    {
        uint64_t vp_num = addr_to_num(vp_addr);
        auto it = table.find(vp_num);
        if (it == table.end())
        {
            std::cerr << "remove: vp_addr " << vp_addr
                      << " (vp_num=" << vp_num << ") not found\n";
            return;
        }
        hash_node *n = it->second;
        n->access_count += 1;
    }

    // -----------------------------------------------------------------------
    // Reset all access_count counters in the LRU list to zero.
    // -----------------------------------------------------------------------
    void reset_counters()
    {
        for (hash_node *cur = head; cur != nullptr; cur = cur->next)
        {
            cur->access_count = 0;
        }
    }

    // -----------------------------------------------------------------------
    // Check if the structure has reached its capacity.
    // -----------------------------------------------------------------------
    bool isFull() const
    {
        return size >= cap;
    }

    // -----------------------------------------------------------------------
    // Look up a node by virtual address. Returns nullptr if not found.
    // -----------------------------------------------------------------------
    hash_node* find_node(uint64_t vp_addr) const
    {
        uint64_t vp_num = addr_to_num(vp_addr);
        auto it = table.find(vp_num);
        return (it == table.end() ? nullptr : it->second);
    }

    // -----------------------------------------------------------------------
    // For debugging: return a vector of all vp_nums in LRU order (MRU first).
    // -----------------------------------------------------------------------
    std::vector<uint64_t> get_nodes() const
    {
        std::vector<uint64_t> v;
        v.reserve(size);
        hash_node *cur = head;
        while (cur)
        {
            v.push_back(cur->vp_num);
            cur = cur->next;
        }
        return v;
    }

    // -----------------------------------------------------------------------
    // For debugging: return the head pointer of the LRU list.
    // -----------------------------------------------------------------------
    hash_node* _datastruct() const
    {
        return head;
    }
    // -------------------------------------------------------------------
    // Splice a node into the MRU position of *this* list.
    // -------------------------------------------------------------------
    void insert_mru_node(hash_node* n) {
        // assumes n is unlinked and not in any list
        n->prev = nullptr;
        n->next = head;
        if (head) head->prev = n;
        head = n;
        if (!tail) tail = n;
        ++size;
        table[n->vp_num] = n;
    }

    // -------------------------------------------------------------------
    // Splice a node into the LRU position of *this* list.
    // -------------------------------------------------------------------
    void insert_lru_node(hash_node* n) {
        // assumes n is unlinked and not in any list
        n->next = nullptr;
        n->prev = tail;
        if (tail) tail->next = n;
        tail = n;
        if (!head) head = n;
        ++size;
        table[n->vp_num] = n;
    }

    // -------------------------------------------------------------------
    // Exchange a “hottest” node from this list with the LRU of another.
    // - candidate = hottest in *this* (compressed)
    // - victim    = LRU in other  (uncompressed)
    // After swap:
    //   candidate goes MRU into other
    //   victim    goes LRU into this
    // -------------------------------------------------------------------
    void swap_with(HashLL& other)
    {
        hash_node* hot  = hottest_node();   // from *this*  (clist)
        hash_node* cold = other.lru_node(); // from other   (unclist)
        if (!hot || !cold) return;

        // --- detach from their original owners ---
        unlink_node(hot);                         // correct: *this*
        table.erase(hot->vp_num);
        --size;

        other.unlink_node(cold);                  // <-- FIX: use *other*
        other.table.erase(cold->vp_num);
        --other.size;

        // --- splice into the opposite lists ---
        other.insert_mru_node(hot);               // hot → unclist (MRU)
        insert_lru_node(cold);                    // cold → clist  (LRU)
    }


private:
    // -----------------------------------------------------------------------
    // Convert full virtual address to virtual page number (vp_num).
    // -----------------------------------------------------------------------
    static uint64_t addr_to_num(uint64_t vp_addr)
    {
        return vp_addr >> 12;  // divide by 4096
    }

    // -----------------------------------------------------------------------
    // Unlink node `n` from the doubly‐linked LRU list (head↔…↔tail).
    // -----------------------------------------------------------------------
    void unlink_node(hash_node *n)
    {
        if (n->prev) n->prev->next = n->next;
        else         head = n->next;

        if (n->next) n->next->prev = n->prev;
        else         tail = n->prev;
    }

    // -----------------------------------------------------------------------
    // Insert node `n` at the head (MRU position) of the LRU list.
    // -----------------------------------------------------------------------
    void insert_at_head(hash_node *n)
    {
        n->prev = nullptr;
        n->next = head;
        if (head) head->prev = n;
        head = n;
        if (!tail) tail = n;
    }

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------
    uint32_t cap;    // maximum number of distinct pages allowed
    uint32_t size;   // current number of pages
    hash_node *head; // MRU (most recent)
    hash_node *tail; // LRU (least recent)

    // Hash map: vp_num → pointer to the node in the LRU list
    std::unordered_map<uint64_t, hash_node*> table;
};

} // namespace HASHLL

#endif /* HASHLL_H */
