#pragma once // Ensure this header is included only once

#include <cstddef>  // For std::size_t, std::ptrdiff_t
#include <cstdint>  // For std::uint8_t, std::uintptr_t
#include <cstring>  // For std::strncpy, std::memset in example
#include <iostream> // For std::cout/std::cerr for logging
#include <mutex>    // For std::mutex and std::lock_guard for thread safety
#include <new>      // For placement new (used by std::allocate_shared)
#include <typeinfo> // For typeid

// -----------------------------------------------------------------------------
// Configuration Macros for the Pool Allocator
// -----------------------------------------------------------------------------

/**
 * @brief Default size of each memory block in the pool (in bytes).
 * This must be chosen carefully. It needs to be large enough to
 * accommodate the object type (T) plus any control block overhead
 * added by std::shared_ptr when using std::allocate_shared.
 * A typical shared_ptr control block is 16-32 bytes.
 * Ensure it's a multiple of `alignof(std::max_align_t)` for best practice.
 */
#ifndef POOL_BLOCK_SIZE
#define POOL_BLOCK_SIZE 64 // Default size for x86_64, adjust as needed
#endif

/**
 * @brief Default number of blocks in the pool.
 * This determines the total memory reserved for the pool: POOL_BLOCK_SIZE *
 * POOL_NUM_BLOCKS.
 */
#ifndef POOL_NUM_BLOCKS
#define POOL_NUM_BLOCKS 100 // Default number of blocks
#endif

// -----------------------------------------------------------------------------
// Internal Structures and Global Mutex
// -----------------------------------------------------------------------------

/**
 * @brief Union used to manage free list nodes and ensure proper alignment.
 * When a block is free, `next` stores a pointer to the next free block.
 * When a block is allocated, `data` is the memory region used by the object.
 * `alignas(std::max_align_t)` ensures the memory is aligned for any standard
 * type.
 */
union FreeNode {
  FreeNode *next; ///< Pointer to the next free node in the list
  alignas(std::max_align_t) std::uint8_t
      data[POOL_BLOCK_SIZE]; ///< Raw memory for the allocated object
};

/**
 * @brief Global mutex for thread-safe access to the free list.
 * A single mutex protects all operations on the pool allocator.
 * For multiple distinct pools, you might define a mutex per pool.
 */
std::mutex g_pool_allocator_mutex;

// -----------------------------------------------------------------------------
// pool_allocator Class Definition
// -----------------------------------------------------------------------------

/**
 * @brief A fixed-size block pool allocator for x86_64 systems.
 *
 * This allocator is designed to conform to the C++ Standard Library's
 * `std::Allocator` concept, making it compatible with `std::allocate_shared`.
 * It manages a contiguous block of memory, dividing it into fixed-size chunks.
 *
 * @tparam T The type of object this allocator is designed to allocate.
 * `std::allocate_shared` will request memory sufficient for `T` and its
 * associated shared_ptr control block.
 * @tparam BlockSize The fixed size (in bytes) of each memory block managed by
 * this pool.
 * @tparam NumBlocks The total number of fixed-size blocks in the pool.
 */
template <typename T, std::size_t BlockSize = POOL_BLOCK_SIZE,
          std::size_t NumBlocks = POOL_NUM_BLOCKS>
class pool_allocator {
public:
  // -------------------------------------------------------------------------
  // Required Standard Allocator Type Definitions
  // -------------------------------------------------------------------------
  using value_type = T; ///< The type of object managed by this allocator
  using pointer = T *;  ///< Pointer to an object
  using const_pointer = const T *;   ///< Constant pointer to an object
  using reference = T &;             ///< Reference to an object
  using const_reference = const T &; ///< Constant reference to an object
  using size_type = std::size_t;     ///< Type for sizes and counts
  using difference_type =
      std::ptrdiff_t; ///< Type for differences between pointers

  /**
   * @brief Required `rebind` struct for `std::allocator_traits`.
   * Allows the allocator to be re-bound to a different type `U`.
   */
  template <typename U> struct rebind {
    using other = pool_allocator<U, BlockSize, NumBlocks>;
  };

  // -------------------------------------------------------------------------
  // Constructors and Assignment Operators
  // -------------------------------------------------------------------------

  /**
   * @brief Default constructor for pool_allocator.
   * Initializes the memory pool and sets up the free list.
   * This initialization happens only once across all instances of a specific
   * `pool_allocator<T, BlockSize, NumBlocks>` configuration.
   */
  pool_allocator() {
    // Use a static flag and double-check locking to ensure one-time
    // initialization
    static bool initialized = false;
    if (!initialized) {
      std::lock_guard<std::mutex> lock(g_pool_allocator_mutex); // Acquire lock
      if (!initialized) { // Double-check inside lock
        FreeNode *current_node = nullptr;
        // Link all blocks into a free list, starting from the end of the buffer
        for (std::size_t i = 0; i < NumBlocks; ++i) {
          // Calculate address of the current block
          FreeNode *node =
              reinterpret_cast<FreeNode *>(&s_pool_buffer[i * BlockSize]);
          // Link it to the current head of the free list
          node->next = current_node;
          // Make this node the new head
          current_node = node;
        }
        s_free_list_head = current_node; // Set the global free list head
        initialized = true;              // Mark as initialized
        std::cout << "pool_allocator: Pool initialized with " << NumBlocks
                  << " blocks of " << BlockSize
                  << " bytes each. Total: " << (NumBlocks * BlockSize)
                  << " bytes." << std::endl;
      }
    }
  }

  /**
   * @brief Copy constructor for allocator rebinding.
   * This allows `std::allocator_traits` to rebind the allocator to a different
   * type `U`. Stateless allocators typically have trivial copy constructors.
   * @tparam U The type of the other allocator.
   * @tparam OtherBlockSize Block size of the other allocator.
   * @tparam OtherNumBlocks Number of blocks of the other allocator.
   * @param other The other allocator instance.
   */
  template <typename U, std::size_t OtherBlockSize, std::size_t OtherNumBlocks>
  pool_allocator(
      const pool_allocator<U, OtherBlockSize, OtherNumBlocks> &other) {
    // No state to copy, as the pool and free list are static/global.
    // The constructor's primary role is to trigger static initialization if not
    // already done. We only copy if the configuration matches (same BlockSize
    // and NumBlocks). This implicitly assumes different template instantiations
    // manage distinct pools.
  }

  /**
   * @brief Deleted copy assignment operator.
   * Allocators are typically stateless and should not be assigned.
   */
  pool_allocator &operator=(const pool_allocator &) = delete;

  // -------------------------------------------------------------------------
  // Allocation and Deallocation Methods
  // -------------------------------------------------------------------------

  /**
   * @brief Allocates a block of raw memory from the pool.
   * @param n The number of `value_type` objects to allocate. For a fixed-size
   * pool allocator used with `std::allocate_shared`, `n` is effectively 1
   * for the object itself, but the underlying allocation request
   * may be `sizeof(T)` plus the shared_ptr control block overhead.
   * This function checks if the requested total size (`n * sizeof(T)`)
   * can fit into a single `BlockSize`.
   * @return A pointer to the allocated memory block, or `nullptr` if no block
   * is available or the requested size is too large for a single block.
   */
  pointer allocate(size_type n) {
    // Critical check: ensure the total size required (for N objects of type T)
    // fits within a single block. std::allocate_shared asks for one "chunk"
    // that contains both the object and its control block.
    // It's vital that POOL_BLOCK_SIZE is sufficiently large for this.
    if (n * sizeof(T) > BlockSize) {
      std::cerr << "pool_allocator ERROR: Requested size (" << (n * sizeof(T))
                << " bytes) for type " << typeid(T).name()
                << " exceeds block size (" << BlockSize << " bytes)."
                << std::endl;
      return nullptr; // Indicate allocation failure
    }

    std::lock_guard<std::mutex> lock(g_pool_allocator_mutex); // Acquire mutex
    FreeNode *allocated_block = s_free_list_head;
    if (allocated_block) {
      s_free_list_head =
          allocated_block->next; // Move head to the next free block
    } else {
      std::cerr << "pool_allocator ERROR: Out of memory in pool for type "
                << typeid(T).name() << "! No free blocks available."
                << std::endl;
      // Optionally throw std::bad_alloc if you want to strictly adhere to
      // std::allocator behavior throw std::bad_alloc();
    }
    return reinterpret_cast<pointer>(
        allocated_block); // Return the allocated raw memory
  }

  /**
   * @brief Deallocates a previously allocated block of memory back to the pool.
   * @param p A pointer to the memory block to deallocate. This pointer must
   * have been obtained from a previous call to `allocate` on this same pool.
   * @param n The number of `value_type` objects (same as passed to `allocate`).
   */
  void deallocate(pointer p, size_type n) {
    // Basic sanity check: ensure the pointer is within the bounds of our static
    // pool. This helps catch accidental deallocations of memory not managed by
    // this pool.
    std::uintptr_t ptr_val = reinterpret_cast<std::uintptr_t>(p);
    std::uintptr_t pool_start = reinterpret_cast<std::uintptr_t>(s_pool_buffer);
    std::uintptr_t pool_end = pool_start + (NumBlocks * BlockSize);

    if (ptr_val < pool_start || ptr_val >= pool_end) {
      std::cerr << "pool_allocator WARNING: Deallocating address " << p
                << " not managed by this pool! Ignoring." << std::endl;
      return; // Do not attempt to deallocate foreign memory
    }

    // Optional: Check if the deallocated block is correctly aligned and on a
    // block boundary. This can catch issues where `p` is not exactly the start
    // of a block.
    if ((ptr_val - pool_start) % BlockSize != 0) {
      std::cerr << "pool_allocator WARNING: Deallocating unaligned address "
                << p << ". Possible corruption. Ignoring." << std::endl;
      return;
    }

    std::lock_guard<std::mutex> lock(g_pool_allocator_mutex); // Acquire mutex
    FreeNode *returned_block = reinterpret_cast<FreeNode *>(p);
    returned_block->next = s_free_list_head; // Add the returned block to the
                                             // front of the free list
    s_free_list_head = returned_block;       // Update the free list head
  }

  // -------------------------------------------------------------------------
  // Equality and Inequality Operators (Required for C++11 and later)
  // -------------------------------------------------------------------------

  /**
   * @brief Equality comparison for allocators.
   * For stateless allocators managing a global/static pool, they are typically
   * considered equal if they refer to the same pool configuration (BlockSize,
   * NumBlocks).
   * @tparam U The type of the other allocator.
   * @tparam OtherBlockSize Block size of the other allocator.
   * @tparam OtherNumBlocks Number of blocks of the other allocator.
   * @param other The other allocator instance to compare with.
   * @return True if the allocators are considered equal, false otherwise.
   */
  template <typename U, std::size_t OtherBlockSize, std::size_t OtherNumBlocks>
  bool operator==(
      const pool_allocator<U, OtherBlockSize, OtherNumBlocks> &other) const {
    return BlockSize == OtherBlockSize && NumBlocks == OtherNumBlocks;
  }

  /**
   * @brief Inequality comparison for allocators.
   * @tparam U The type of the other allocator.
   * @tparam OtherBlockSize Block size of the other allocator.
   * @tparam OtherNumBlocks Number of blocks of the other allocator.
   * @param other The other allocator instance to compare with.
   * @return True if the allocators are considered unequal, false otherwise.
   */
  template <typename U, std::size_t OtherBlockSize, std::size_t OtherNumBlocks>
  bool operator!=(
      const pool_allocator<U, OtherBlockSize, OtherNumBlocks> &other) const {
    return !(*this == other);
  }

private:
  // -------------------------------------------------------------------------
  // Static Members (Shared by all instances of a specific template
  // instantiation)
  // -------------------------------------------------------------------------

  /**
   * @brief The head of the free list of memory blocks.
   * This pointer points to the first available free block in the pool.
   */
  static FreeNode *s_free_list_head;

  /**
   * @brief The raw memory buffer that forms the memory pool.
   * This is a static array, meaning its memory is allocated in the data/BSS
   * segment of the program. This makes it persistent for the lifetime of the
   * application.
   */
  static alignas(std::max_align_t) std::uint8_t
      s_pool_buffer[BlockSize * NumBlocks];
};

// -----------------------------------------------------------------------------
// Static Member Definitions (Must be outside the class definition)
// -----------------------------------------------------------------------------

// Initialize the static free list head to nullptr.
template <typename T, std::size_t BlockSize, std::size_t NumBlocks>
FreeNode *pool_allocator<T, BlockSize, NumBlocks>::s_free_list_head = nullptr;

// Define and (implicitly) zero-initialize the static memory pool buffer.
// `alignas` ensures the entire buffer is properly aligned.
template <typename T, std::size_t BlockSize, std::size_t NumBlocks>
alignas(std::max_align_t) std::uint8_t pool_allocator<
    T, BlockSize, NumBlocks>::s_pool_buffer[BlockSize * NumBlocks];
