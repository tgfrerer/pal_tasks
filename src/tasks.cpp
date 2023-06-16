#include "tasks.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "lockfree_ring_buffer.h"

using coroutine_handle_t = std::coroutine_handle<TaskPromise>;


// A channel is a thread-safe primitive to communicate with worker threads - 
// each worker thread has exactly one channel. We use one channel per worker
// thread to feed coroutine handles to the worker thread. Once a channel contains
// a payload it is blocked (you cannot push anymore handles onto this channel).
// The channel gets free and ready to receive another handle as soon as the 
// worker thread has finished processing the current handle.
struct Channel {
	void*                  handle; // storage for channel payload: one single handle. void means that the channel is free.
	std::atomic_flag       flag; // signal that the current channel is busy.

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

	~Channel() {

		// Once the channel accepts a coroutine handle, it becomes the 
		// owner of the handle. If there are any leftover valid handles 
		// that we own when this object dips into ovlivion, we must clean
		// them up first.
		//
		if ( this->handle ) {
			//std::cout << "WARNING: leftover task in channel." << std::endl;
			//std::cout << "destroying task: " << this->handle << std::endl;
			Task::from_address( this->handle ).destroy();
		}

		this->handle = nullptr;
	}
};

class task_list_o {
	lockfree_ring_buffer_t tasks;
	std::atomic_size_t     num_tasks; // number of tasks, only gets decremented if taks has been removed

  public:
	std::atomic_flag block_flag ; // flag used to signal that dependent tasks have completed

	task_list_o( uint32_t capacity_hint = 32 ) // start with capacity of 32
	    : tasks( capacity_hint )
	    , num_tasks( 0 ) {
	}

	~task_list_o() {
		// If there are any tasks left on the task list, we must destroy them, as we own them.
		void* task;
		while ( ( task = this->tasks.try_pop() ) ) {
			Task::from_address( task ).destroy();
		}
	}

	// Push a suspended task back onto the end of the task list
	inline void push_task( coroutine_handle_t const& c ) {
		tasks.push( c.address() );
	}

	// Get the next task if possible, if there is no next task, 
	// return an empty coroutine handle. 
	// An empty coroutine handle will compare true to nullptr
	inline coroutine_handle_t pop_task() {
		return Task::from_address( tasks.try_pop() );
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

TaskList::TaskList( uint32_t hint_capacity )
    : p_impl( new task_list_o( hint_capacity ) ) {
}

void TaskList::add_task( Task c ) {
	assert( p_impl != nullptr && "task list must be valid. Was this task list already used?" );
	p_impl->add_task( c );
}

TaskList::~TaskList() {
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
	std::vector<Channel*>     channels; // non-owning - channels are owned by their threads
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
	void wait_for_task_list( TaskList& p_t );
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
		channels.emplace_back( new Channel() );
		threads.emplace_back(
		    //
		    // Thread worker implementation
		    //
		    []( std::stop_token stop_token, Channel* ch ) {
			    while ( !stop_token.stop_requested() ) {

				    if ( ch->handle ) {
					    coroutine_handle_t::from_address( ch->handle ).resume(); // resume coroutine
					    ch->handle = nullptr;
					    // signal that we are ready to receive new tasks
					    ch->flag.clear( std::memory_order_release );
					    continue;
				    }

				    // Wait for flag to be set
				    //
				    // The flag is set on any of the following:
				    //   * A new job has been placed in the channel
				    // 	 * The current task list is empty.
				    ch->flag.wait( false, std::memory_order_acquire );
			    }

			    // Channel is owned by the thread - when the thread falls out of scope
			    // that means that the channel gets deleted, too.
			    delete ch;
		    },
		    channels.back() );
	}
}

void scheduler_impl::wait_for_task_list( TaskList& p_t ) {

	if ( p_t.p_impl == nullptr ) {
		assert( false && "Task list must have been freshly initialised. Has this task list been waited for already?" );
		return;
	}

	// --------| Invariant: TaskList is valid

	// Execute tasks in this task list until there are no more tasks left
	// to execute.

	// Before we start executing tasks we must take ownership of them
	// by tagging them so that they know which scheduler they belong to.
	p_t.p_impl->tag_all_tasks_with_scheduler( this );

	// Distribute work, as long as there is work to distribute

	while ( p_t.p_impl->get_tasks_count() ) {

		// ----------| Invariant: There are Tasks in this Task List which have not yet completed

		coroutine_handle_t c = ( p_t.p_impl )->pop_task();

		if ( c == nullptr ) {

			// We could not fetch a task from the task list - this means 
			// that there are tasks in-progress that we must wait for.

			if ( p_t.p_impl->block_flag.test_and_set() ) {
				std::cout << "blocking thread " << std::this_thread::get_id() << " on [" << p_t.p_impl << "]" << std::endl;
				// Wait for the flag to be set - this is the case if any of these happen:
				//    * the scheduler is destroyed
				//    * the last task of the task list has completed, and the task list is now empty.
				p_t.p_impl->block_flag.wait( true );
				std::cout << "resuming thread " << std::this_thread::get_id() << " on [" << p_t.p_impl << "]" << std::endl;
			} else {
				std::cout << "spinning thread " << std::this_thread::get_id() << " on [" << p_t.p_impl << "]" << std::endl;
			}

			continue;
		}

		// ----------| Invariant: current coroutine is valid

		// Find a free channel. if there is, then place this handle in the channel,
		// which means that it will be executed on the worker thread associated
		// with this channel.

		if ( move_task_to_worker_thread( c ) ) {
			// Pushing consumes the coroutine handle - that is, it becomes owned by the channel
			// who owns it for the worker thread.
			//
			// If we made it in here, the handle was successfully offloaded to a worker thread.
			//
			// The worker thread must now execute the payload, and the task will decrement the 
			// counter for the current TaskList upon completion.
			continue;
		}

		// --------| Invariant: All worker threads are busy - or there are no worker threads: we must execute on this thread
		c();
	}

	// Once all tasks have been complete, release task list
	delete p_t.p_impl;    // Free task list impl
	p_t.p_impl = nullptr; // Signal to any future users that this task list has been used already
}

// ----------------------------------------------------------------------

inline bool scheduler_impl::move_task_to_worker_thread( coroutine_handle_t& c ) {
	// Iterate over all channels. If we can place the coroutine
	// on a channel, do so.
	for ( auto& ch : channels ) {
		if ( true == ch->try_push( c ) ) {
			return true;
		}
	}
	return false;
}

// ----------------------------------------------------------------------

Scheduler::Scheduler( int32_t num_worker_threads )
    : p_impl( new scheduler_impl( num_worker_threads ) ) {
}

void Scheduler::wait_for_task_list( TaskList& p_t ) {
	p_impl->wait_for_task_list( p_t );
}

Scheduler* Scheduler::create( int32_t num_worker_threads ) {
	return new Scheduler( num_worker_threads );
}

Scheduler::~Scheduler() {
	delete p_impl;
}

// ----------------------------------------------------------------------

void defer_task::await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept {

	// ----------| Invariant: At this point the coroutine pointed to by h 
	// has been fully suspended. This is guaranteed by the c++ standard.

	// Check if we have a scheduler available via the promise.
	//
	// If not, this means we have not been placed onto the scheduler,
	// and we should not start execution yet.
	if ( h.promise().scheduler ) {

		// If there is a scheduler available, this means that the current
		// coroutine has been suspended mid-way.

		auto& task_list = h.promise().p_task_list;

		// Put the current coroutine to the back of the scheduler queue
		// as it has been fully suspended at this point.

		task_list->push_task( h.promise().get_return_object() );

		// --- Eager Workers --- 
		// 
		// Eagerly try to fetch & execute the next task from the front of the 
		// scheduler queue -
		// We do this so that multiple threads can share the
		// scheduling workload.
		//
		// But we can also disable that, so that there is only one thread
		// that does the scheduling, and removing elements from the
		// queue.

		coroutine_handle_t c = task_list->pop_task();

		if ( c && !c.done() ) {
			c();
		} else {
			assert( false && "task must not be done" );
		}
	}

	// Note: Once we drop off here, controy will return to where the resume() 
	// command that brought us here was issued.
}

// ----------------------------------------------------------------------

void finalize_task::await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept {
	// This is the last time that this coroutine will be awakened
	// we do not suspend it anymore after this
	h.promise().p_task_list->decrement_task_count();
	h.destroy(); // are we allowed to destroy here?
}
