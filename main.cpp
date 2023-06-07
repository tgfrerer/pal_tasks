#include "tasks.h"
#include <iostream>

#include <thread>
//

int main() {
	// we would like a syntax that goes:

	scheduler_o* scheduler = scheduler_o::create( -1 ); // create a scheduler with two hardware worker threads

	srand( 0xdeadbeef );

	std::cout << "MAIN thread is: " << std::hex << std::this_thread::get_id() << std::endl;

	if ( false ) {

		task_list_t task_list{};
		auto        coro_generator = []( int i ) -> task {
            std::cout << "executing coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
                      << std::flush;

            std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 20 ) );
			// this yields control5 back to the await_suspend method, and to our scheduler
			co_await schedule_task();

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
		/*
		 * This test is for whether we can issue tasks from within our
		 * current task system.
		 *
		 */

		task_list_t another_task_list{};
		auto        coro_generator = []( int i, scheduler_o* sched ) -> task {
            std::cout << "first level coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
                      << std::flush;

            std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 20 ) );

			task_list_t inner_task_list{};

			auto inner_coro_generator = []( int i, int j ) -> task {
				std::cout << "\t executing inner coroutine: " << std::dec << i << ":" << j++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl
                          << std::flush;

                std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 10 ) );
				// this yields control back to the await_suspend method, and to our scheduler
				co_await schedule_task();

				std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 20 ) );
				std::cout << "\t executing inner coroutine: " << std::dec << i << ":" << j++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
				co_return;
			};

			for ( int j = 0; j != 40; j++ ) {
				inner_task_list.add_task( inner_coro_generator( i, j * 10 ) );
			}

			sched->wait_for_task_list( inner_task_list );

			co_await schedule_task();
			// this yields control back to the await_suspend method, and to our scheduler

			std::cout << "executing first level coroutine: " << std::dec << i++ << " on thread: " << std::hex << std::this_thread::get_id() << std::endl;
			std::this_thread::sleep_for( std::chrono::milliseconds( rand() % 20 ) );

			co_return;
		};

		for ( int i = 0; i != 30; i++ ) {
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
