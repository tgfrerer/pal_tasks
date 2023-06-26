#ifndef _LOCK_FREE_RING_BUFFER_H_
#define _LOCK_FREE_RING_BUFFER_H_

/*
 * Based on the work of Brian Watling for libfiber - see
 * <https://github.com/brianwatling/libfiber>
 *
 * libfiber is:
 *
 * Copyright (c) 2012-2015, Brian Watling and other contributors
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <malloc.h>
#include <atomic>
#include <vector>
#include <thread>

static_assert( std::atomic_size_t::is_always_lock_free, "atomic_size_t must be always lock free" );

inline uint32_t next_power_of_2( uint32_t v ) {
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return ++v;
}

class lockfree_ring_buffer_t {
	// high and low are generally used together; no point putting them on separate cache lines
	std::atomic_size_t m_high;
	char               _cache_padding1[ 64 - sizeof( std::atomic_size_t ) ];
	std::atomic_size_t m_low;
	char               _cache_padding2[ 64 - sizeof( std::atomic_size_t ) ];
	uint32_t           m_capacity;
	uint32_t           m_power_of_2_mod;
	// buffer must be last - it spills outside of this struct
	std::vector<void*> buffer;

	lockfree_ring_buffer_t( const lockfree_ring_buffer_t& )            = delete;
	lockfree_ring_buffer_t( lockfree_ring_buffer_t&& )                 = delete;
	lockfree_ring_buffer_t& operator=( const lockfree_ring_buffer_t& ) = delete;
	lockfree_ring_buffer_t& operator=( lockfree_ring_buffer_t&& )      = delete;

  public:
	lockfree_ring_buffer_t( uint32_t power_of_2_size )
	    : m_capacity( next_power_of_2( power_of_2_size ) )
	    , m_power_of_2_mod( m_capacity - 1 )
	    , buffer( m_capacity, nullptr ) {
		assert( power_of_2_size );
	}
	size_t size() {
		// read high first; make it look less than or equal to its actual size
		const uint64_t high = this->m_high.load( std::memory_order_acquire );
		// load_load_barrier();
		const int64_t size = high - this->m_low.load( std::memory_order_acquire );
		return size >= 0 ? size : 0;
	}

	int try_push( void* in ) {
		assert( in ); // can't store NULLs; we rely on a NULL to indicate a spot in the buffer has not been written yet
		// read low first; this means the buffer will appear larger or equal to its actual size
		const uint64_t low = this->m_low.load( std::memory_order_acquire );
		// load_load_barrier();
		uint64_t       high  = this->m_high.load( std::memory_order_acquire );
		const uint64_t index = high & this->m_power_of_2_mod;
		if ( !this->buffer[ index ] &&
		     high - low < this->m_capacity &&
		     this->m_high.compare_exchange_weak( high, high + 1, std::memory_order_release ) ) {
			this->buffer[ index ] = in;
			return 1;
		}
		return 0;
	}

	void push( void* in ) {
		while ( !this->try_push( in ) ) {
			if ( this->m_high - this->m_low >= this->m_capacity ) {
				// the buffer is full - we must block...
				std::this_thread::sleep_for( std::chrono::nanoseconds( 100 ) );
			}
		}
	}

	void* try_pop() {
		// read high first; this means the buffer will appear smaller or equal to its actual size
		const uint64_t high = this->m_high.load( std::memory_order_acquire );
		// load_load_barrier();
		uint64_t       low   = this->m_low.load( std::memory_order_acquire );
		const uint64_t index = low & this->m_power_of_2_mod;
		void* const    ret   = this->buffer[ index ];
		if ( ret &&
		     high > low &&
		     this->m_low.compare_exchange_weak( low, low + 1, std::memory_order_release ) ) {
			this->buffer[ index ] = nullptr;
			return ret;
		}
		return nullptr;
	}

	void* pop() {
		void* ret;
		while ( !( ret = try_pop() ) ) {
			if ( this->m_high <= this->m_low ) {
				// cpu_relax();//the buffer is empty
				std::this_thread::sleep_for( std::chrono::nanoseconds( 100 ) );
			}
		}
		return ret;
	}

	// ----------------------------------------------------------------------
	// The following methods are specialisations only useful in the context
	// of our job system - we know that while we set up at tasklist we don't
	// have any other thread competing for access and are therefore safe to
	// do direct access, and re-allocations.
	// ----------------------------------------------------------------------

	// ONLY SAFE IN SINGLE_THREADED ENVIRONMENT
	// Dynamically grows the buffer if necessary. You must not do this once
	// there is more than one thread modifying the buffer, as it is then unsafe.
	int unsafe_initial_dynamic_push( void* in ) {
		assert( m_low == 0 && "you must not unsafe push once an item has been popped from this array" );
		if ( this->m_high - this->m_low >= this->m_capacity ) {
			this->m_capacity *= 2; // double size
			this->buffer.resize( m_capacity, nullptr );
			this->m_power_of_2_mod = this->m_capacity - 1;
		}
		return try_push( in );
	}

	// ONLY SAFE IN SINGLE_THREADED ENVIRONMENT
	// iterates over all elements in the data array and applies the
	// call back function
	void unsafe_for_each( void ( *fun )( void* item, void* user_data ), void* user_data ) {
		size_t low  = this->m_low;
		size_t high = this->m_high;
		for ( size_t i = low; i != high; i++ ) {
			fun( buffer[ i ], user_data );
		}
	}
};

#endif
