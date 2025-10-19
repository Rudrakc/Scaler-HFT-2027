#pragma once
#include <cstdint>
#include <vector>
#include <map>
#include <unordered_map>
#include <list>
#include <memory>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstring>

// Order structure
struct Order
{
    uint64_t order_id;
    bool is_buy;
    double price;
    uint64_t quantity;
    uint64_t timestamp_ns;
};

// Price level aggregation
struct PriceLevel
{
    double price;
    uint64_t total_quantity;
};

// Memory pool for efficient allocation
template <typename T, size_t BlockSize = 4096>
class MemoryPool
{
private:
    struct Block
    {
        alignas(T) char data[sizeof(T) * BlockSize];
        Block *next;
    };

    Block *head_block;
    std::vector<std::unique_ptr<Block>> blocks;
    std::vector<T *> free_list;
    size_t current_index;

public:
    MemoryPool() : head_block(nullptr), current_index(BlockSize)
    {
        allocate_block();
    }

    ~MemoryPool() = default;

    T *allocate()
    {
        if (!free_list.empty())
        {
            T *ptr = free_list.back();
            free_list.pop_back();
            return ptr;
        }

        if (current_index >= BlockSize)
        {
            allocate_block();
        }

        return reinterpret_cast<T *>(&head_block->data[sizeof(T) * current_index++]);
    }

    void deallocate(T *ptr)
    {
        if (ptr)
        {
            ptr->~T();
            free_list.push_back(ptr);
        }
    }

private:
    void allocate_block()
    {
        auto new_block = std::make_unique<Block>();
        new_block->next = head_block;
        head_block = new_block.get();
        blocks.push_back(std::move(new_block));
        current_index = 0;
    }
};

// Internal order representation with list iterator for O(1) removal
struct OrderNode
{
    Order order;
    std::list<OrderNode *>::iterator level_iterator;

    OrderNode(const Order &o) : order(o) {}
};

// Price level with FIFO order queue
struct Level
{
    double price;
    uint64_t total_quantity;
    std::list<OrderNode *> orders; // FIFO queue

    Level(double p) : price(p), total_quantity(0) {}
};

class OrderBook
{
private:
    // Memory pool for order allocation
    MemoryPool<OrderNode, 1024> order_pool;

    // Price levels sorted by price (descending for bids, ascending for asks)
    std::map<double, Level, std::greater<double>> bid_levels; // Highest first
    std::map<double, Level, std::less<double>> ask_levels;    // Lowest first

    // O(1) order lookup
    std::unordered_map<uint64_t, OrderNode *> order_lookup;

    // Statistics
    mutable uint64_t total_orders = 0;
    mutable uint64_t total_cancels = 0;
    mutable uint64_t total_amends = 0;

public:
    OrderBook() = default;

    ~OrderBook()
    {
        // Clean up all orders
        for (auto &[id, node] : order_lookup)
        {
            order_pool.deallocate(node);
        }
    }

    // Insert a new order into the book
    void add_order(const Order &order)
    {
        // Allocate new order node from pool
        OrderNode *node = order_pool.allocate();
        new (node) OrderNode(order);

        // Add to lookup table
        order_lookup[order.order_id] = node;

        // Add to appropriate side
        if (order.is_buy)
        {
            add_to_side(bid_levels, node);
        }
        else
        {
            add_to_side(ask_levels, node);
        }

        total_orders++;
    }

    // Cancel an existing order by its ID
    bool cancel_order(uint64_t order_id)
    {
        auto it = order_lookup.find(order_id);
        if (it == order_lookup.end())
        {
            return false;
        }

        OrderNode *node = it->second;

        // Remove from appropriate side
        if (node->order.is_buy)
        {
            remove_from_side(bid_levels, node);
        }
        else
        {
            remove_from_side(ask_levels, node);
        }

        // Remove from lookup and deallocate
        order_lookup.erase(it);
        order_pool.deallocate(node);

        total_cancels++;
        return true;
    }

    // Amend an existing order's price or quantity
    bool amend_order(uint64_t order_id, double new_price, uint64_t new_quantity)
    {
        auto it = order_lookup.find(order_id);
        if (it == order_lookup.end())
        {
            return false;
        }

        OrderNode *node = it->second;

        // If price changes, treat as cancel + add
        if (std::abs(node->order.price - new_price) > 1e-9)
        {
            Order new_order = node->order;
            new_order.price = new_price;
            new_order.quantity = new_quantity;
            new_order.timestamp_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();

            cancel_order(order_id);
            add_order(new_order);
        }
        else
        {
            // Only quantity changes - update in place
            if (node->order.is_buy)
            {
                update_quantity_in_place(bid_levels, node, new_quantity);
            }
            else
            {
                update_quantity_in_place(ask_levels, node, new_quantity);
            }
        }

        total_amends++;
        return true;
    }

    // Get a snapshot of top N bid and ask levels
    void get_snapshot(size_t depth, std::vector<PriceLevel> &bids, std::vector<PriceLevel> &asks) const
    {
        bids.clear();
        asks.clear();

        // Get top bids
        size_t count = 0;
        for (const auto &[price, level] : bid_levels)
        {
            if (count++ >= depth)
                break;
            bids.push_back({price, level.total_quantity});
        }

        // Get top asks
        count = 0;
        for (const auto &[price, level] : ask_levels)
        {
            if (count++ >= depth)
                break;
            asks.push_back({price, level.total_quantity});
        }
    }

    // Print current state of the order book
    void print_book(size_t depth = 10) const
    {
        std::vector<PriceLevel> bids, asks;
        get_snapshot(depth, bids, asks);

        std::cout << "\n========== ORDER BOOK ==========\n";
        std::cout << std::fixed << std::setprecision(2);

        // Print asks in reverse (highest price first for visual clarity)
        std::cout << "\n--- ASKS (Sell Orders) ---\n";
        std::cout << std::setw(12) << "Price" << " | " << std::setw(12) << "Quantity\n";
        std::cout << "----------------------------\n";
        for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        {
            std::cout << std::setw(12) << it->price << " | "
                      << std::setw(12) << it->total_quantity << "\n";
        }

        // Print spread
        if (!bids.empty() && !asks.empty())
        {
            double spread = asks.front().price - bids.front().price;
            std::cout << "\n   SPREAD: " << spread << "\n";
        }

        // Print bids
        std::cout << "\n--- BIDS (Buy Orders) ---\n";
        std::cout << std::setw(12) << "Price" << " | " << std::setw(12) << "Quantity\n";
        std::cout << "----------------------------\n";
        for (const auto &level : bids)
        {
            std::cout << std::setw(12) << level.price << " | "
                      << std::setw(12) << level.total_quantity << "\n";
        }

        std::cout << "\n================================\n";
        std::cout << "Total Orders: " << order_lookup.size() << "\n";
        std::cout << "Bid Levels: " << bid_levels.size() << "\n";
        std::cout << "Ask Levels: " << ask_levels.size() << "\n";
    }

    // Get best bid and ask prices (for potential matching)
    std::pair<double, double> get_best_prices() const
    {
        double best_bid = bid_levels.empty() ? 0.0 : bid_levels.begin()->first;
        double best_ask = ask_levels.empty() ? std::numeric_limits<double>::max() : ask_levels.begin()->first;
        return {best_bid, best_ask};
    }

    // Performance statistics
    void print_stats() const
    {
        std::cout << "\n--- Performance Stats ---\n";
        std::cout << "Total Orders Added: " << total_orders << "\n";
        std::cout << "Total Orders Cancelled: " << total_cancels << "\n";
        std::cout << "Total Orders Amended: " << total_amends << "\n";
    }

private:
    template <typename MapType>
    void add_to_side(MapType &side, OrderNode *node)
    {
        auto &level = side[node->order.price];
        level.price = node->order.price;
        level.orders.push_back(node);
        node->level_iterator = std::prev(level.orders.end());
        level.total_quantity += node->order.quantity;
    }

    template <typename MapType>
    void remove_from_side(MapType &side, OrderNode *node)
    {
        auto it = side.find(node->order.price);
        if (it != side.end())
        {
            auto &level = it->second;
            level.orders.erase(node->level_iterator);
            level.total_quantity -= node->order.quantity;

            // Remove empty price level
            if (level.orders.empty())
            {
                side.erase(it);
            }
        }
    }

    template <typename MapType>
    void update_quantity_in_place(MapType &side, OrderNode *node, uint64_t new_quantity)
    {
        auto it = side.find(node->order.price);
        if (it != side.end())
        {
            auto &level = it->second;
            level.total_quantity = level.total_quantity - node->order.quantity + new_quantity;
            node->order.quantity = new_quantity;
        }
    }
};

// Example usage and test harness
class OrderBookTester
{
public:
    static void run_basic_test()
    {
        OrderBook book;

        std::cout << "=== Order Book Test ===\n";

        // Add some buy orders
        book.add_order({1001, true, 100.00, 100, 1000000});
        book.add_order({1002, true, 99.50, 200, 2000000});
        book.add_order({1003, true, 100.00, 150, 3000000}); // Same price as 1001
        book.add_order({1004, true, 98.00, 300, 4000000});

        // Add some sell orders
        book.add_order({2001, false, 101.00, 100, 5000000});
        book.add_order({2002, false, 102.00, 200, 6000000});
        book.add_order({2003, false, 101.00, 150, 7000000}); // Same price as 2001
        book.add_order({2004, false, 103.50, 300, 8000000});

        std::cout << "\nInitial Order Book:\n";
        book.print_book();

        // Test cancel
        std::cout << "\nCancelling order 1002...\n";
        book.cancel_order(1002);
        book.print_book(5);

        // Test amend (quantity only)
        std::cout << "\nAmending order 1003 quantity to 500...\n";
        book.amend_order(1003, 100.00, 500);
        book.print_book(5);

        // Test amend (price change)
        std::cout << "\nAmending order 2001 price to 100.50...\n";
        book.amend_order(2001, 100.50, 100);
        book.print_book(5);

        // Get snapshot
        std::vector<PriceLevel> bids, asks;
        book.get_snapshot(3, bids, asks);

        std::cout << "\nTop 3 Levels Snapshot:\n";
        std::cout << "Bids: ";
        for (const auto &lvl : bids)
        {
            std::cout << "[" << lvl.price << ":" << lvl.total_quantity << "] ";
        }
        std::cout << "\nAsks: ";
        for (const auto &lvl : asks)
        {
            std::cout << "[" << lvl.price << ":" << lvl.total_quantity << "] ";
        }
        std::cout << "\n";

        book.print_stats();
    }

    static void run_performance_test()
    {
        OrderBook book;
        const int num_orders = 100000;

        std::cout << "\n=== Performance Test ===\n";
        std::cout << "Adding " << num_orders << " orders...\n";

        auto start = std::chrono::high_resolution_clock::now();

        // Add orders
        for (int i = 0; i < num_orders; ++i)
        {
            double price = 95.0 + (i % 100) * 0.1;
            bool is_buy = i % 2 == 0;
            book.add_order({static_cast<uint64_t>(i), is_buy, price, 100, static_cast<uint64_t>(i)});
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        std::cout << "Time to add " << num_orders << " orders: "
                  << duration.count() << " microseconds\n";
        std::cout << "Average per order: "
                  << duration.count() / static_cast<double>(num_orders) << " microseconds\n";

        // Test snapshot performance
        start = std::chrono::high_resolution_clock::now();
        std::vector<PriceLevel> bids, asks;
        for (int i = 0; i < 1000; ++i)
        {
            book.get_snapshot(10, bids, asks);
        }
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

        std::cout << "Time for 1000 snapshots: " << duration.count() << " microseconds\n";
        std::cout << "Average per snapshot: "
                  << duration.count() / 1000.0 << " microseconds\n";
    }
};

// Main function for testing
#ifdef ORDERBOOK_MAIN
int main()
{
    OrderBookTester::run_basic_test();
    OrderBookTester::run_performance_test();
    return 0;
}
#endif