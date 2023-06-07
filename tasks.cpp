#include "tasks.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "lockfree_ring_buffer.h"

using coroutine_handle_t = std::coroutine_handle<promise>;


struct channel {
	// add channel where we can block until there is something to read
	void*                  handle;
	std::atomic_flag       flag{ false }; // signal that the current thread is busy.

	bool try_push( coroutine_handle_t& h ) {

		if ( flag.test_and_set( std::memory_order_acquire ) ) {
			// if the current channel was already flagged
			// we cannot add anymore work.
			return false;
		}

		// --------| invariant: current channel is available now

		handle = h.address();

		// If there is a thread blocked on this operation, we
		// unblock it here.
		flag.notify_one();

		return true;
	}

	~channel() {
		// we must cleanup any leftover tasks.

		if ( this->handle ) {
			std::cout << "WARNING: leftover task in channel." << std::endl;
			std::cout << "destroying task: " << this->handle << std::endl;
			task::from_address( this->handle ).destroy();
		}

		this->handle = nullptr;

		std::cout << "deleting channel." << std::endl;
	}
};

class task_list_o {
	lockfree_ring_buffer_t tasks;
	std::atomic_size_t     num_tasks; // number of tasks, only gets decremented if taks has been removed

  public:
	std::atomic_flag block_flag = { false }; // flag used to signal that dependent tasks have completed

	task_list_o( uint32_t capacity_hint = 32 ) // start with capacity of 32
	    : tasks( capacity_hint )
	    , num_tasks( 0 ) {
	}

	~task_list_o() {
		// if there are any tasks left on the task list, we must destroy them, as we own them.
		std::cout << "Destroying task list" << std::endl;
		void* task;
		while ( ( task = this->tasks.try_pop() ) ) {
			std::cout << "Destroying task: " << task << std::endl;
			task::from_address( task ).destroy();
		}
	}

	// push a suspended task back onto the end of the task list
	inline void push_task( coroutine_handle_t const& c ) {
		// std::cout << "pushed task: " << c.address() << std::endl;
		tasks.push( c.address() );
	}

	// Get the next task if possible,
	// if there is no next task, return an empty coroutine handle
	// an empty coroutine handle will compare true to nullptr
	inline coroutine_handle_t pop_task() {
		// invariant: tasks is not empty
		return task::from_address( tasks.try_pop() );
	}

	// Return the number of tasks which are both in flight and waiting
	//
	// Note this is not the same as tasks.size() as any tasks which are being
	// processed and are in flight will not show up on the task list.
	//
	// num_tasks gets decremented only if a task was fully completed.
	inline size_t get_tasks_count() {
		return num_tasks;
	}


	// Add a new task to the task list - only allowed in setup phase,
	// where only one thread has access to the task list.
	void add_task( coroutine_handle_t& c ) {
		c.promise().p_task_list = this;
		tasks.unsafe_initial_dynamic_push( c.address() );
		num_tasks++;
		// std::cout << "added task: " << c.address() << std::endl;
	}

	void tag_all_tasks_with_scheduler( scheduler_impl* p_scheduler ) {
		tasks.unsafe_for_each(
		    []( void* c, void* p_scheduler ) {
			    coroutine_handle_t::from_address( c ).promise().scheduler = static_cast<scheduler_impl*>( p_scheduler );
		    },
		    p_scheduler );
	}

	void decrement_task_count() {
		size_t num_flags = --num_tasks;
		if ( num_flags == 0 ) {
			block_flag.clear();
			block_flag.notify_one(); // unblock us on block flag.
		}
	}
};

task_list_t::task_list_t( uint32_t hint_capacity )
    : p_impl( new task_list_o( hint_capacity ) ) {
}

void task_list_t::add_task( task c ) {
	assert( p_impl != nullptr && "task list must be valid. Was this task list already used?" );
	p_impl->add_task( c );
}

task_list_t::~task_list_t() {
	// In case that this task list was deleted already, p_impl will
	// be nullptr, which means that this delete operator is a no-op.
	// otherwise (in case a tasklist has not been used and needs to
	// be cleaned up), this will perform the cleanup for us.
	delete p_impl;
}

// ----------------------------------------------------------------------

class scheduler_impl {

	bool move_task_to_worker_thread( coroutine_handle_t& c );

  public:
	std::vector<channel*>     channels; // non-owning - channels are owned by their threads
	std::vector<std::jthread> threads;

	scheduler_impl( int32_t num_worker_threads = 0 );

	~scheduler_impl() {
		// We must unblock any threads which are currently waiting on a flag signal for more work
		// as there is no more work coming, we must artificially signal the flag so that these
		// worker threads can resume to completion.
		for ( auto* c : channels ) {
			if ( c ) {
				c->flag.test_and_set(); // Set flag so that if there is a worker blocked on this flag, it may proceed.
				c->flag.notify_one();   // Notify the worker thread (if any worker thread is waiting) that the flag has flipped.
				                        // without notify, waiters will not be notified that the flag has flipped.
			}
		}
		// We must wait until all the threads have been joined.
		// Deleting a jthread object implicitly stops (sets the stop_token) and joins.
		threads.clear();
	}

	// Execute all tasks in the task list, then invalidate the task list object
	void wait_for_task_list( task_list_t& p_t );
};

scheduler_impl::scheduler_impl( int32_t num_worker_threads ) {

	if ( num_worker_threads < 0 ) {
		// If negative, then this means that we must
		// count backwards from the number of available hardware threads
		num_worker_threads = std::jthread::hardware_concurrency() + num_worker_threads;
	}

	assert( num_worker_threads >= 0 && "Inferred number of worker threads must not be negative" );

	std::cout << "Initializing scheduler with " << num_worker_threads << " worker threads" << std::endl;

	// Reserve memory so that we can take addresses for channel,
	// and don't have to worry about iterator validity
	channels.reserve( num_worker_threads );
	threads.reserve( num_worker_threads );

	// NOTE THAT BY DEFAULT WE DON'T HAVE ANY WORKER THREADS
	//
	for ( int i = 0; i != num_worker_threads; i++ ) {
		channels.emplace_back( new channel() );
		threads.emplace_back(
		    //
		    // Thread worker implementation
		    //
		    []( std::stop_token stop_token, channel* ch ) {
			    while ( !stop_token.stop_requested() ) {

				    if ( ch->handle ) {
					    coroutine_handle_t::from_address( ch->handle ).resume(); // resume coroutine
					    ch->handle = nullptr;
					    // signal that we are ready to receive new tasks
					    ch->flag.clear( std::memory_order_release );
					    continue;
				    }

				    // Wait for flag to be set - which means that a new job has been placed in the channel
				    // or that the current tasklist has completed.
				    ch->flag.wait( false, std::memory_order_acquire );
				    // std::cout << "flag unblocked" << std::endl;
			    }

			    // Channel is owned by the thread - when the thread falls out of scope
			    // that means that the channel gets deleted, too.
			    delete ch;
		    },
		    channels.back() );
	}
}

void scheduler_impl::wait_for_task_list( task_list_t& p_t ) {

	if ( p_t.p_impl == nullptr ) {
		assert( false && "Task list must have been initialised. Has this task list been waited for already?" );
		return;
	}

	// --------| Invariant: task list is valid

	// Execute tasks in this task list until there are no more tasks left
	// to execute.

	// Before we start executing tasks we must take ownership of them
	// by tagging them so that they know which scheduler they belong to.
	p_t.p_impl->tag_all_tasks_with_scheduler( this );

	// Distribute work, as long as there is work to distribute
	while ( p_t.p_impl->get_tasks_count() ) {
		coroutine_handle_t c = ( p_t.p_impl )->pop_task();

		if ( c == nullptr ) {
			// This thread is starved of work -- we must wait for the worker threads to complete
			// before we can progress further.
			if ( p_t.p_impl->block_flag.test_and_set() ) {
				std::cout << "blocking main thread on [" << p_t.p_impl << "]" << std::endl;
				p_t.p_impl->block_flag.wait( true ); // Signalled whenever the task count decreases
				std::cout << "resuming main thread on [" << p_t.p_impl << "]" << std::endl;
			}
			continue;
		}

		// ----------| invariant: current coroutine is valid

		// Find a free channel. if there is, then place this handle in the channel,
		// which means that it will be executed on the worker thread associated
		// with this channel.

		if ( move_task_to_worker_thread( c ) ) {
			// pushing consumes the coroutine handle - that is, it becomes owned by the channel
			// who owns it for the worker thread.
			// handle was successfully offloaded to a worker thread.
			// the worker thread must now execute the payload, and
			// the task will  decrement the counter for this tasklist
			// upon completion.
			continue;
		}

		// --------| Invariant: All worker threads are busy - we must execute on this thread

		assert( c );
		c();
	}

	// Once all tasks have been complete, release task list
	delete p_t.p_impl;    // Free task list impl
	p_t.p_impl = nullptr; // Signal any future uses that this task list has been used already
}

// ----------------------------------------------------------------------

inline bool scheduler_impl::move_task_to_worker_thread( coroutine_handle_t& c ) {
	// Iterate over all channels. if we can place the coroutine
	// on a channel, do so.
	for ( auto& ch : channels ) {
		if ( true == ch->try_push( c ) ) {
			return true;
		}
	}
	return false;
}

// ----------------------------------------------------------------------

scheduler_o::scheduler_o( int32_t num_worker_threads )
    : p_impl( new scheduler_impl( num_worker_threads ) ) {
}

void scheduler_o::wait_for_task_list( task_list_t& p_t ) {
	p_impl->wait_for_task_list( p_t );
}

scheduler_o* scheduler_o::create( int32_t num_worker_threads ) {
	return new scheduler_o( num_worker_threads );
}

scheduler_o::~scheduler_o() {
	delete p_impl;
}

// ----------------------------------------------------------------------

void schedule_task::await_suspend( std::coroutine_handle<promise> h ) noexcept {
	// ----------| Invariant: At this point the coroutine pointed
	// to by h has been fully suspended. This is guaranteed by the Standard.

	// Check if we have a scheduler available via the promise.
	//
	// If not, we have not been placed onto the scheduler,
	// and we should not start execution yet.
	if ( h.promise().scheduler ) {
		auto& scheduler = h.promise().scheduler;
		auto& task_list = h.promise().p_task_list;
		// put the current coroutine to the back of the scheduler queue
		// as it has been fully suspended at this point.

		task_list->push_task( h.promise().get_return_object() );

		// enables or disables eager work-stealing
		if ( true ) {

			// take next task from front of scheduler queue -
			// we can do this so that multiple threads can share the
			// scheduling workload potentially.
			//
			// but we can also disable that, so that there is only one thread
			// that does the scheduling, and that removes elements from the
			// queue.

			coroutine_handle_t c = task_list->pop_task();

			if ( c && !c.done() ) {
				c();
			} else {
				assert( false && "task must not be done" );
			}
		}
	}

	// Note: If we drop off here, we must do so whilst being in the scheduling thread -
	// as this will return to where the resume() command was issued.
}

// ----------------------------------------------------------------------

void finalize_task::await_suspend( std::coroutine_handle<promise> h ) noexcept {
	// This is the last time that this coroutine will be awakened
	// we do not suspend it anymore after this
	h.promise().p_task_list->decrement_task_count();
	std::cout << "Final suspend for coroutine." << std::endl
	          << std::flush;

	h.destroy(); // are we allowed to destroy here?
}
