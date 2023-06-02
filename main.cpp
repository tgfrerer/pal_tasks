#include "tasks.h"
#include <iostream>

#include <thread>
//

int main() {
	// we would like a syntax that goes:

	scheduler_o* scheduler = scheduler_o::create( 1 ); // create a scheduler with two hardware worker threads

	srand( 0xdeadbeef );

	std::cout << "MAIN thread is: " << std::hex << std::this_thread::get_id() << std::endl;

	if ( false ) {

		task_list_t task_list{};
		auto        coro_generator = []( int i ) -> task {
            std::cout << "executing coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
                      << std::flush;

            std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 20 ) );
            // this yields control5 back to the await_suspend method, and to our scheduler
            co_await scheduled_task();

            std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 20 ) );
            std::cout << "executing coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
            co_return;
		};

		for ( int i = 0; i != 100; i++ ) {
			task_list.add_task( coro_generator( i * 100 ) );
		}

		scheduler->wait_for_task_list( task_list );
	}

	if ( true ) {
		/* this test is for whether we can issue tasks from within our
		 * current task system.
		 *
		 * What would we expect? we would expect execution to happen
		 * in parallel, we would expect tasks to only complete once their
		 * subtasks have completed.
		 *
		 * we would expect tasks that are on the same level to execute in parallel.
		 *
		 * Current bug: if a coroutine schedules one more task,
		 * then the channel / worker thread which issues the new task remains blocked.
		 *
		 * this is because no worker will take on more work once it has started to process work.
		 *
		 * what would be the correct way to work around this?
		 *
		 */

		task_list_t another_task_list{};
		auto        coro_generator = []( int i, scheduler_o* sched ) -> task {
            std::cout << "first level coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
                      << std::flush;

            std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 20 ) );

            task_list_t inner_task_list{};

            auto inner_coro_generator = []( int i ) -> task {
                std::cout << "\t executing inner coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
                          << std::flush;

                std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 10 ) );
                // this yields control back to the await_suspend method, and to our scheduler
                co_await scheduled_task();

                std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 20 ) );
                std::cout << "\t executing inner coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
                co_return;
            };

            for ( int i = 0; i != 1; i++ ) {
                inner_task_list.add_task( inner_coro_generator( i * 10 ) );
            }

            sched->wait_for_task_list( inner_task_list );

            // this yields control back to the await_suspend method, and to our scheduler
            co_await scheduled_task();

            std::cout << "executing first level coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
            std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 20 ) );

            co_return;
		};

		for ( int i = 0; i != 1; i++ ) {
			another_task_list.add_task( coro_generator( i * 10, scheduler ) );
		}

		std::cout << "main program starts wait for task list." << std::endl
		          << std::flush;

		scheduler->wait_for_task_list( another_task_list );
	}

	std::cout << "Back with main program." << std::endl
	          << std::flush;
	delete scheduler;

	return 0;
}
