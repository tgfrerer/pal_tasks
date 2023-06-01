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

#include "lockfree_ring_buffer.h"

#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <malloc.h>
#include <atomic>
#include <thread>

static_assert( std::atomic_size_t::is_always_lock_free, "atomic_size_t must be always lock free" );

struct lockfree_ring_buffer_t {
	// high and low are generally used together; no point putting them on separate cache lines
	std::atomic_size_t high;
	char               _cache_padding1[ 64 - sizeof( std::atomic_size_t ) ];
	std::atomic_size_t low;
	char               _cache_padding2[ 64 - sizeof( std::atomic_size_t ) ];
	uint32_t           size;
	uint32_t           power_of_2_mod;
	// buffer must be last - it spills outside of this struct
	void* buffer[];
};

lockfree_ring_buffer_t* lockfree_ring_buffer_create( uint32_t power_of_2_size ) {
	assert( power_of_2_size && power_of_2_size < 32 );
	const uint32_t                size          = 1 << power_of_2_size;
	const uint32_t                required_size = sizeof( lockfree_ring_buffer_t ) + size * sizeof( void* );
	lockfree_ring_buffer_t* const ret           = ( lockfree_ring_buffer_t* )calloc( 1, required_size );
	if ( ret ) {
		ret->size           = size;
		ret->power_of_2_mod = size - 1;
	}
	return ret;
}

void lockfree_ring_buffer_destroy( lockfree_ring_buffer_t* rb ) {
	free( rb );
}

size_t lockfree_ring_buffer_size( const lockfree_ring_buffer_t* rb ) {
	assert( rb );
	// read high first; make it look less than or equal to its actual size
	const uint64_t high = rb->high.load( std::memory_order_acquire );
	// load_load_barrier();
	const int64_t size = high - rb->low.load( std::memory_order_acquire );
	return size >= 0 ? size : 0;
}

int lockfree_ring_buffer_trypush( lockfree_ring_buffer_t* rb, void* in ) {
	assert( rb );
	assert( in ); // can't store NULLs; we rely on a NULL to indicate a spot in the buffer has not been written yet
	// read low first; this means the buffer will appear larger or equal to its actual size
	const uint64_t low = rb->low.load( std::memory_order_acquire );
	// load_load_barrier();
	uint64_t       high  = rb->high.load( std::memory_order_acquire );
	const uint64_t index = high & rb->power_of_2_mod;
	if ( !rb->buffer[ index ] &&
	     high - low < rb->size &&
	     rb->high.compare_exchange_weak( high, high + 1, std::memory_order_release ) ) {
		rb->buffer[ index ] = in;
		return 1;
	}
	return 0;
}

void lockfree_ring_buffer_push( lockfree_ring_buffer_t* rb, void* in ) {
	while ( !lockfree_ring_buffer_trypush( rb, in ) ) {
		if ( rb->high - rb->low >= rb->size ) {
			// the buffer is full - we must block...
			std::this_thread::sleep_for( std::chrono::nanoseconds( 100 ) );
		}
	}
}

void* lockfree_ring_buffer_trypop( lockfree_ring_buffer_t* rb ) {
	assert( rb );
	// read high first; this means the buffer will appear smaller or equal to its actual size
	const uint64_t high = rb->high.load( std::memory_order_acquire );
	// load_load_barrier();
	uint64_t       low   = rb->low.load( std::memory_order_acquire );
	const uint64_t index = low & rb->power_of_2_mod;
	void* const    ret   = rb->buffer[ index ];
	if ( ret &&
	     high > low &&
	     rb->low.compare_exchange_weak( low, low + 1, std::memory_order_release ) ) {
		rb->buffer[ index ] = nullptr;
		return ret;
	}
	return nullptr;
}

void* lockfree_ring_buffer_pop( lockfree_ring_buffer_t* rb ) {
	void* ret;
	while ( !( ret = lockfree_ring_buffer_trypop( rb ) ) ) {
		if ( rb->high <= rb->low ) {
			// cpu_relax();//the buffer is empty
			std::this_thread::sleep_for( std::chrono::nanoseconds( 100 ) );
		}
	}
	return ret;
}
