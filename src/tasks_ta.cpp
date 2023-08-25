#include "tasks.h"
#include <cassert>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "lockfree_ring_buffer.h"
#include "sched.h"
#include <mutex>

static constexpr auto WORKER_EAGERNESS = 1; // how many tasks to try to load into current worker on every iteration

// non-thread-safe implementation of ring buffer - use this if you know for sure that only one thread
// will ever access this data.
class ring_buffer_t {
	// high and low are generally used together; no point putting them on separate cache lines
	uint32_t           m_high;
	uint32_t           m_low;
	uint32_t           m_capacity;
	uint32_t           m_power_of_2_mod;
	std::vector<void*> buffer;

	ring_buffer_t( const ring_buffer_t& )            = delete;
	ring_buffer_t( ring_buffer_t&& )                 = delete;
	ring_buffer_t& operator=( const ring_buffer_t& ) = delete;
	ring_buffer_t& operator=( ring_buffer_t&& )      = delete;

  public:
	ring_buffer_t( uint32_t power_of_2_size )
	    : m_capacity( next_power_of_2( power_of_2_size ) )
	    , m_power_of_2_mod( m_capacity - 1 )
	    , buffer( m_capacity, nullptr ) {
		assert( power_of_2_size );
	}
	size_t size() {
		const int64_t size = m_high - m_low;
		return size >= 0 ? size : 0;
	}

	void push( void* in ) {
		assert( in ); // can't store NULLs; we rely on a NULL to indicate a spot in the buffer has not been written yet
		// read low first; this means the buffer will appear larger or equal to its actual size
		const uint32_t index = m_high & this->m_power_of_2_mod;
		if ( !this->buffer[ index ] && m_high - m_low < this->m_capacity ) {
			this->buffer[ index ] = in;
			this->m_high++;
		}
	}

	void* try_pop() {
		// read high first; this means the buffer will appear smaller or equal to its actual size
		const uint64_t index = m_low & this->m_power_of_2_mod;
		void* const    ret   = this->buffer[ index ];
		if ( ret && m_high > m_low ) {
			this->buffer[ index ] = nullptr;
			m_low++;
			return ret;
		}
		return nullptr;
	}
};

using coroutine_handle_t = std::coroutine_handle<TaskPromise>;
struct work_queue_t {
	// wow! if there is only one thread accessing this structure, it is almost free.
	lockfree_ring_buffer_t priority_0{ 4096 }; // workload for this worker at priority 0
};

/* *
 *
 * */

struct Channel {

	// we want to be able to add to the current channel whenever we want
	// whatever we add to the current channel becomes owned by the current channel -
	// everything that we add is a move operation

	// multiple threads may add to the channel, only one thread (the channel's thread itself)
	// will ever remove from the channel - unless we allow for work-stealing.

	std::atomic_flag flag      = false; // signal that the current channel is busy.
	scheduler_impl*  scheduler = nullptr;
	work_queue_t     workload{};

	~Channel() {
		// if we own any leftover work items, we must destroy these here
		//
		void* t = workload.priority_0.try_pop();
		while ( t ) {
			assert( false && "Leftover tasks in channel - this should not happen" );
			coroutine_handle_t::from_address( t ).destroy();
			t = workload.priority_0.try_pop();
		}
	}
};

class task_list_o {
	alignas( 64 ) std::atomic_size_t task_count; // Number of tasks, only gets decremented if task has been completed
	lockfree_ring_buffer_t tasks;                //
	coroutine_handle_t     waiting_task;         // If this task list is waited upon, this will be the task to resume once all tasks have completed
  public:

	task_list_o( uint32_t capacity_hint = 32 ) // start with capacity of 32
	    : tasks( capacity_hint )
	    , task_count( 0 ) {
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


	// Add a new task to the task list - only allowed in setup phase,
	// where only one thread has access to the task list.
	void add_task( coroutine_handle_t& c ) {
		c.promise().p_task_list = this;
		tasks.unsafe_initial_dynamic_push( c.address() );
		++task_count;
	}

	void tag_all_tasks_with_scheduler( scheduler_impl* p_scheduler ) {
		tasks.unsafe_for_each(
		    []( void* c, void* p_scheduler ) {
			    coroutine_handle_t::from_address( c ).promise().scheduler = static_cast<scheduler_impl*>( p_scheduler );
		    },
		    p_scheduler );
	}

	inline size_t get_task_count() {
		return task_count;
	}

	void decrement_task_count() {
		//
		// ACHTUNG: From the moment on that task_count goes to zero
		// you must, like any most pessimistic soul, assume that *this*
		// object has been deleted by another thread, eager to clean up.
		// From that moment on therefore any reference to `this` is a
		// potential use-after-free...
		//
		// What's still guaranteed to be around, however, is our stack frame,
		// and this is why we capture a local copy of `waiting_task` before
		// we potentially decrement `task_count`.
		//
		//
		//
		// you cannot assume that the object is still alive here.
		coroutine_handle_t waiting_task_local = this->waiting_task;

		if ( 0 == --task_count && waiting_task_local ) {
			// resume the scope that was waiting on this task list -
			// potentially deleting *this* - you must not access `this`
			// anymore after `resume`!
			waiting_task_local.resume();
			return;
		}
		// *** DO NOT ACCESS `this` anymore here as it may have been deleted. ***
	}

	friend struct await_tasks;
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

	lockfree_ring_buffer_t m_task_queue{ 4096 }; // space for 4096 tasks
	work_queue_t           workload;           // work queue for workloads executed on the same thread as the scheduler

  public:
	std::vector<Channel*>     channels; // non-owning - channels are owned by their threads
	std::vector<std::jthread> threads;

	scheduler_impl( int32_t num_worker_threads = 0 );

	~scheduler_impl() {

		// We must request all threads to stop, as they will shortly be destroyed.
		//
		// We must then nudge any threads which are currently waiting on a flag signal
		// so that they get a chance to notice that they have been stopped - otherwise
		// they will wait to infinity.

		for ( auto* c : channels ) {
			if ( c ) {
				c->flag.test_and_set(); // Set flag so that if there is a worker blocked on this flag, it may proceed.
				c->flag.notify_one();   // Notify the worker thread (if any worker thread is waiting) that the flag has flipped.
				                        // without notify, waiters will not be notified that the flag has flipped.
			}
		}

		for ( auto& t : threads ) {
			t.request_stop();
		}


		// We must wait until all the threads have been joined.
		// Deleting a jthread object implicitly stops (sets the stop_token) and joins.
		threads.clear();
	}

	// Execute all tasks in the task list, then invalidate the task list object
	void wait_for_task_list( TaskList& p_t );

	// Execute all tasks in the task list, then invalidate the task list object.
	// This version of the function returns an awaitable.
	await_tasks wait_for_task_list_inner( TaskList& p_t );

	// gets all tasks from a task list and appends them to this scheduler's task queue
	bool add_tasks( task_list_o* task_list );

	friend struct await_tasks;
};

scheduler_impl::scheduler_impl( int32_t num_worker_threads ) {

	if ( num_worker_threads < 0 ) {
		// If negative, then this means that we must
		// count backwards from the number of available hardware threads
		num_worker_threads = std::jthread::hardware_concurrency() + num_worker_threads;
	}

	assert( num_worker_threads >= 0 && "Inferred number of worker threads must not be negative" );

	// std::cout << "Initializing scheduler with " << num_worker_threads << " worker threads" << std::endl;

	// Reserve memory so that we can take addresses for channel,
	// and don't have to worry about iterator validity
	channels.reserve( num_worker_threads );
	threads.reserve( num_worker_threads );

	static std::mutex debug_print_mutex;

	// NOTE THAT BY DEFAULT WE DON'T HAVE ANY WORKER THREADS
	//
	cpu_set_t cpuset;
	for ( int i = 0; i != num_worker_threads; i++ ) {
		channels.emplace_back( new Channel() );
		channels.back()->scheduler = this;
		threads.emplace_back(
		    //
		    // Thread worker implementation
		    //
		    []( std::stop_token stop_token, Channel* ch ) {
			    // Sleep until flag gets set - we sleep so that
			    // there is an opportunity for the scheduler to
			    // apply thread cpu affinity.
			    ch->flag.wait( false );

			    if ( 1 ) {
				    auto lock = std::scoped_lock( debug_print_mutex );
				    std::cout << "New worker thread on CPU " << sched_getcpu()
				              << std::endl;
			    }

			    while ( !stop_token.stop_requested() ) {
				    // std::cout << "Executing on cpu: " << sched_getcpu()
				    //           << std::endl;

				    void* t = nullptr;

				    // try pulling in a batch of new work from the scheduler
				    for ( int i = 0; i != WORKER_EAGERNESS; i++ ) {
					    t = ch->scheduler->m_task_queue.try_pop();
					    if ( t == nullptr ) {
						    // no more work in this scheduler.
						    if ( 0 == ch->workload.priority_0.size() ) {
							    // if there are no tasks left on neither the channel nor the scheduler,
							    // then we should probably sleep this worker.
						    }
						    break;
					    } else {
						    // tag the task so that it belongs to the current channel
						    coroutine_handle_t::from_address( t ).promise().p_work_queue = &ch->workload;
						    ch->workload.priority_0.push( t );
					    }
				    }

				    t = ch->workload.priority_0.try_pop();

				    if ( t ) {
					    coroutine_handle_t::from_address( t ).resume(); // resume coroutine
				    }

			    }

			    // Channel is owned by the thread - when the thread falls out of scope
			    // that means that the channel gets deleted, too.
			    delete ch;
		    },
		    channels.back() );

		uint32_t hardware_concurrency = std::jthread::hardware_concurrency();
		CPU_ZERO( &cpuset );
		CPU_SET( i % hardware_concurrency, &cpuset );
		// this really makes a difference.
		bool err = pthread_setaffinity_np( threads.back().native_handle(), sizeof( cpuset ), &cpuset );
		assert( err == 0 && "SetAffinity did not work." );
	}

	// notify all channels that their flags have been set -

	for ( auto* c : channels ) {
		if ( c ) {
			c->flag.test_and_set(); // Set flag so that if there is a worker blocked on this flag, it may proceed.
			c->flag.notify_one();   // Notify the worker thread (if any worker thread is waiting) that the flag has flipped.
			                        // without notify, waiters will not be notified that the flag has flipped.
		}
	}
}

// ----------------------------------------------------------------------

void scheduler_impl::wait_for_task_list( TaskList& tl ) {

	task_list_o* task_list = tl.p_impl;
	tl.p_impl              = nullptr; // we take possession of this task list

	if ( task_list == nullptr ) {
		assert( false && "Task list must have been freshly initialised. Has this task list been waited for already?" );
		return;
	}

	// --------| Invariant: TaskList is valid

	// Before we start executing tasks we must take ownership of them
	// by tagging them so that they know which scheduler they belong to.
	task_list->tag_all_tasks_with_scheduler( this );

	// Add current task list to the list of task lists that are owned by this scheduler.

	while ( false == this->add_tasks( task_list ) ) {

		// Retry adding tasks if we couldn't - this is most likely because
		// the scheduler buffer was full.
		std::cout << "CAN'T ADD MORE TASKS" << std::endl;
		std::this_thread::sleep_for( std::chrono::nanoseconds( 100 ) );
	};

	// Distribute work, as long as there is work to distribute

	while ( task_list->get_task_count() ) {

		// ----------| Invariant: There are Tasks in this Task List which have not yet completed

		void* t = this->workload.priority_0.try_pop();

		if ( t ) {
			coroutine_handle_t::from_address( t ).resume(); // resume coroutine
			// signal that we are ready to receive new tasks
			t = nullptr;
		}

		// Try pulling in a batch of new work from the scheduler
		for ( int i = 0; i != WORKER_EAGERNESS; i++ ) {
			t = this->m_task_queue.try_pop();
			if ( t == nullptr ) {
				// no more work in this scheduler.
				if ( 0 == this->workload.priority_0.size() ) {
					// if there are no tasks left on neither the channel nor the scheduler,
					// then we should probably sleep this worker.
				}
				break;
			} else {
				// tag the task so that it belongs to the current channel
				coroutine_handle_t::from_address( t ).promise().p_work_queue = &this->workload;
				this->workload.priority_0.push( t );
			}
		}
	}

	// Once all tasks have been complete, release task list
	delete task_list;
}

// ----------------------------------------------------------------------

await_tasks scheduler_impl::wait_for_task_list_inner( TaskList& tl ) {
	if ( nullptr == tl.p_impl ) {
		assert( false && "Task list must have been freshly initialised. Has this task list been waited for already?" );
		return {};
	}
	// --------| Invariant: TaskList is valid

	// Tag all tasks with their scheduler,
	// so that they know where they belong to
	tl.p_impl->tag_all_tasks_with_scheduler( this );

	// create an awaitable that can own a tasklist

	await_tasks result;
	result.p_task_list = tl.p_impl;

	// await_tasks::await_suspend is where control will go next.
	return result;
}
// ----------------------------------------------------------------------

void await_tasks::await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept {
	// The task that waits on a task list is now fully suspended.

	// who will wake it up?
	// it gets resumed once the last element in its tasklist has completed.

	task_list_o* p_task_list = this->p_task_list;

	if ( 0 == p_task_list->get_task_count() ) {
		// If there are no tasks left in this task list, we can immediately resume
		h.resume();
	} else {

		// Resume this coroutine once all tasks have completed.
		// Note that the thread that picks up the continuation may not be the same as
		// the thread that initiated the wait as whoever completes the last task of the
		// task list will get to resume this coroutine.
		p_task_list->waiting_task = h;

		// If there are tasks, we move them to the scheduler.
		while ( false == h.promise().scheduler->add_tasks( p_task_list ) ) {
			// retry adding tasks if we couldn't - this is most likely because
			// the scheduler buffer was full.
			std::cout << "CAN'T ADD MORE TASKS" << std::endl;
			std::this_thread::sleep_for( std::chrono::microseconds( 10 ) );
		}
	}
}
// ----------------------------------------------------------------------

bool scheduler_impl::add_tasks( task_list_o* task_list ) {
	coroutine_handle_t t            = task_list->pop_task();
	while ( t ) {
		if ( this->m_task_queue.try_push( t.address() ) ) {
			t = task_list->pop_task();
		} else {
			// could not push t onto task queue - we must place it back onto
			// the queue where it came from so that someone else can pick it
			// up later.
			task_list->push_task( t );
			return false;
		}
	}
	return true;
}

// ----------------------------------------------------------------------

Scheduler::Scheduler( int32_t num_worker_threads )
    : p_impl( new scheduler_impl( num_worker_threads ) ) {
}

void Scheduler::wait_for_task_list( TaskList& p_t ) {
	p_impl->wait_for_task_list( p_t );
}

await_tasks Scheduler::wait_for_task_list_inner( TaskList& p_t ) {
	return this->p_impl->wait_for_task_list_inner( p_t );
}

Scheduler* Scheduler::create( int32_t num_worker_threads ) {
	return new Scheduler( num_worker_threads );
}

Scheduler::~Scheduler() {
	delete p_impl;
}

// ----------------------------------------------------------------------

void suspend_task::await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept {

	// ----------| Invariant: At this point the coroutine pointed to by h
	// has been fully suspended. This is guaranteed by the c++ standard.

	auto& promise = h.promise();

	// push the task back onto the queue where it came from.
	promise.p_work_queue->priority_0.push( h.address() );

	// Note: Once we drop off here, control will return to where the resume()
	// command that brought us here was issued.
}

// ----------------------------------------------------------------------

void finalize_task::await_suspend( std::coroutine_handle<TaskPromise> h ) noexcept {
	// This is the last time that this coroutine will be awakened
	// we do not suspend it anymore after this
	h.promise().p_task_list->decrement_task_count();
	// ---
	h.destroy();
}
