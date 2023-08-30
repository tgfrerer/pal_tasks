// [header]
// A very basic raytracer example.
// [/header]
// [compile]
// c++ -o raytracer -O3 -Wall raytracer.cpp
// [/compile]
// [ignore]
// Copyright (C) 2012  www.scratchapixel.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
// [/ignore]
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <vector>
#include <iostream>
#include <cassert>

#include <thread>
#include "src/lockfree_ring_buffer.h"

#include "src/tasks.h"

#if defined __linux__ || defined __APPLE__
// "Compiled for Linux
#else
// Windows doesn't define these values by default, Linux does
#define M_PI 3.141592653589793
#define INFINITY 1e8
#endif

template<typename T>
class Vec3
{
public:
    T x, y, z;
    Vec3() : x(T(0)), y(T(0)), z(T(0)) {}
    Vec3(T xx) : x(xx), y(xx), z(xx) {}
    Vec3(T xx, T yy, T zz) : x(xx), y(yy), z(zz) {}
    Vec3& normalize()
    {
        T nor2 = length2();
        if (nor2 > 0) {
            T invNor = 1 / sqrt(nor2);
            x *= invNor, y *= invNor, z *= invNor;
        }
        return *this;
    }
    Vec3<T> operator * (const T &f) const { return Vec3<T>(x * f, y * f, z * f); }
    Vec3<T> operator * (const Vec3<T> &v) const { return Vec3<T>(x * v.x, y * v.y, z * v.z); }
    T dot(const Vec3<T> &v) const { return x * v.x + y * v.y + z * v.z; }
    Vec3<T> operator - (const Vec3<T> &v) const { return Vec3<T>(x - v.x, y - v.y, z - v.z); }
    Vec3<T> operator + (const Vec3<T> &v) const { return Vec3<T>(x + v.x, y + v.y, z + v.z); }
    Vec3<T>& operator += (const Vec3<T> &v) { x += v.x, y += v.y, z += v.z; return *this; }
    Vec3<T>& operator *= (const Vec3<T> &v) { x *= v.x, y *= v.y, z *= v.z; return *this; }
    Vec3<T> operator - () const { return Vec3<T>(-x, -y, -z); }
    T length2() const { return x * x + y * y + z * z; }
    T length() const { return sqrt(length2()); }
    friend std::ostream & operator << (std::ostream &os, const Vec3<T> &v)
    {
        os << "[" << v.x << " " << v.y << " " << v.z << "]";
        return os;
    }
};

typedef Vec3<float> Vec3f;

class Sphere
{
public:
    Vec3f center;                           /// position of the sphere
    float radius, radius2;                  /// sphere radius and radius^2
    Vec3f surfaceColor, emissionColor;      /// surface color and emission (light)
    float transparency, reflection;         /// surface transparency and reflectivity
    Sphere(
        const Vec3f &c,
        const float &r,
        const Vec3f &sc,
        const float &refl = 0,
        const float &transp = 0,
        const Vec3f &ec = 0) :
        center(c), radius(r), radius2(r * r), surfaceColor(sc), emissionColor(ec),
        transparency(transp), reflection(refl)
    { /* empty */ }
    //[comment]
    // Compute a ray-sphere intersection using the geometric solution
    //[/comment]
    bool intersect(const Vec3f &rayorig, const Vec3f &raydir, float &t0, float &t1) const
    {
        Vec3f l = center - rayorig;
        float tca = l.dot(raydir);
        if (tca < 0) return false;
        float d2 = l.dot(l) - tca * tca;
        if (d2 > radius2) return false;
        float thc = sqrt(radius2 - d2);
        t0 = tca - thc;
        t1 = tca + thc;
        
        return true;
    }
};

//[comment]
// This variable controls the maximum recursion depth
//[/comment]
#define MAX_RAY_DEPTH 9

float mix(const float &a, const float &b, const float &mix)
{
    return b * mix + a * (1 - mix);
}

//[comment]
// This is the main trace function. It takes a ray as argument (defined by its origin
// and direction). We test if this ray intersects any of the geometry in the scene.
// If the ray intersects an object, we compute the intersection point, the normal
// at the intersection point, and shade this point using this information.
// Shading depends on the surface property (is it transparent, reflective, diffuse).
// The function returns a color for the ray. If the ray intersects an object that
// is the color of the object at the intersection point, otherwise it returns
// the background color.
//[/comment]
Vec3f trace(
    const Vec3f &rayorig,
    const Vec3f &raydir,
    const std::vector<Sphere> &spheres,
    const int &depth)
{
    //if (raydir.length() != 1) std::cerr << "Error " << raydir << std::endl;
    float tnear = INFINITY;
    const Sphere* sphere = NULL;
    // find intersection of this ray with the sphere in the scene
    for (unsigned i = 0; i < spheres.size(); ++i) {
        float t0 = INFINITY, t1 = INFINITY;
        if (spheres[i].intersect(rayorig, raydir, t0, t1)) {
            if (t0 < 0) t0 = t1;
            if (t0 < tnear) {
                tnear = t0;
                sphere = &spheres[i];
            }
        }
    }
    // if there's no intersection return black or background color
    if (!sphere) return Vec3f(2);
    Vec3f surfaceColor = 0; // color of the ray/surfaceof the object intersected by the ray
    Vec3f phit = rayorig + raydir * tnear; // point of intersection
    Vec3f nhit = phit - sphere->center; // normal at the intersection point
    nhit.normalize(); // normalize normal direction
    // If the normal and the view direction are not opposite to each other
    // reverse the normal direction. That also means we are inside the sphere so set
    // the inside bool to true. Finally reverse the sign of IdotN which we want
    // positive.
    float bias = 1e-4; // add some bias to the point from which we will be tracing
    bool inside = false;
    if (raydir.dot(nhit) > 0) nhit = -nhit, inside = true;
    if ((sphere->transparency > 0 || sphere->reflection > 0) && depth < MAX_RAY_DEPTH) {
        float facingratio = -raydir.dot(nhit);
        // change the mix value to tweak the effect
        float fresneleffect = mix(pow(1 - facingratio, 3), 1, 0.1);
        // compute reflection direction (not need to normalize because all vectors
        // are already normalized)
        Vec3f refldir = raydir - nhit * 2 * raydir.dot(nhit);
        refldir.normalize();
        Vec3f reflection = trace(phit + nhit * bias, refldir, spheres, depth + 1);
        Vec3f refraction = 0;
        // if the sphere is also transparent compute refraction ray (transmission)
        if (sphere->transparency) {
            float ior = 1.1, eta = (inside) ? ior : 1 / ior; // are we inside or outside the surface?
            float cosi = -nhit.dot(raydir);
            float k = 1 - eta * eta * (1 - cosi * cosi);
            Vec3f refrdir = raydir * eta + nhit * (eta *  cosi - sqrt(k));
            refrdir.normalize();
            refraction = trace(phit - nhit * bias, refrdir, spheres, depth + 1);
        }
        // the result is a mix of reflection and refraction (if the sphere is transparent)
        surfaceColor = (
            reflection * fresneleffect +
            refraction * (1 - fresneleffect) * sphere->transparency) * sphere->surfaceColor;
    }
    else {
        // it's a diffuse object, no need to raytrace any further
        for (unsigned i = 0; i < spheres.size(); ++i) {
            if (spheres[i].emissionColor.x > 0) {
                // this is a light
                Vec3f transmission = 1;
                Vec3f lightDirection = spheres[i].center - phit;
                lightDirection.normalize();
                for (unsigned j = 0; j < spheres.size(); ++j) {
                    if (i != j) {
                        float t0, t1;
                        if (spheres[j].intersect(phit + nhit * bias, lightDirection, t0, t1)) {
                            transmission = 0;
                            break;
                        }
                    }
                }
                surfaceColor += sphere->surfaceColor * transmission *
                std::max(float(0), nhit.dot(lightDirection)) * spheres[i].emissionColor;
            }
        }
    }
    
    return surfaceColor + sphere->emissionColor;
}

//[comment]
// Main rendering function. We compute a camera ray for each pixel of the image
// trace it and return a color. If the ray hits a sphere, we return the color of the
// sphere at the intersection point, else we return the background color.
//[/comment]
void render( int num_threads, const char* choice, const std::vector<Sphere>& spheres ) {

	static const unsigned width    = 1920 * 2;
	static const unsigned height   = 768 * 2;
	Vec3f*                image    = new Vec3f[ width * height ];
	static const float    invWidth = 1 / float( width ), invHeight = 1 / float( height );
	static const float    fov = 30, aspectratio = width / float( height );
	static const float    angle = tan( M_PI * 0.5 * fov / 180. );
	// Trace rays
	std::cout << "Num Threads: " << num_threads << std::endl;

	if ( choice && *choice == '1' ) {
		// use scheduler
		choice++;

		Scheduler* scheduler = Scheduler::create( num_threads );
		if ( choice && *choice == '1' ) {

			std::cout << "Coroutines: One Task List for all pixels" << std::endl;

			TaskBuffer tl( width * height );
			for ( unsigned y = 0; y < height; ++y ) {
				for ( unsigned x = 0; x < width; ++x ) {
					Task t = []( unsigned x, unsigned y, Vec3f* image, std::vector<Sphere> const& spheres, Scheduler* scheduler ) -> Task {
						float xx = ( 2 * ( ( x + 0.5 ) * invWidth ) - 1 ) * angle * aspectratio;
						float yy = ( 1 - 2 * ( ( y + 0.5 ) * invHeight ) ) * angle;
						Vec3f raydir( xx, yy, -1 );
						raydir.normalize();
						image[ y * width + x ] = trace( Vec3f( 0 ), raydir, spheres, 0 );
						co_return;
					}( x, y, image, spheres, scheduler );

					tl.add_task( t );
				}
			}

			scheduler->wait_for_task_buffer( tl );

		} else {

			// tasklists that wait on task lists

			std::cout << "Coroutines: One Task List per row" << std::endl;

			for ( unsigned y = 0; y < height; ++y ) {
				TaskBuffer tl( width );
				for ( unsigned x = 0; x < width; ++x ) {
					Task t = []( unsigned x, unsigned y, Vec3f* image, std::vector<Sphere> const& spheres, Scheduler* scheduler ) -> Task {
						float xx = ( 2 * ( ( x + 0.5 ) * invWidth ) - 1 ) * angle * aspectratio;
						float yy = ( 1 - 2 * ( ( y + 0.5 ) * invHeight ) ) * angle;
						Vec3f raydir( xx, yy, -1 );
						raydir.normalize();
						image[ y * width + x ] = trace( Vec3f( 0 ), raydir, spheres, 0 );
						co_return;
					}( x, y, image, spheres, scheduler );

					tl.add_task( t );
				}
				scheduler->wait_for_task_buffer( tl );
			}
		}
		delete scheduler;

	} else {
		// do not use scheduler
		choice++;

		if ( choice && *choice == '0' ) {
			std::cout << "Single-Threaded // Baseline" << std::endl;

			for ( unsigned y = 0; y < height; ++y ) {
				for ( unsigned x = 0; x < width; ++x ) {
					float xx = ( 2 * ( ( x + 0.5 ) * invWidth ) - 1 ) * angle * aspectratio;
					float yy = ( 1 - 2 * ( ( y + 0.5 ) * invHeight ) ) * angle;
					Vec3f raydir( xx, yy, -1 );
					raydir.normalize();
					image[ y * width + x ] = trace( Vec3f( 0 ), raydir, spheres, 0 );
				}
			}
		} else {
			std::cout << "Task-Based" << std::endl;

			std::vector<std::jthread> threads;
			std::atomic_size_t        inflight_tasks = width * height;

			threads.reserve( num_threads );

			// each thread may signal that it is done.
			// if a thread is done then it may look for some more work
			// if there is not more work, we want to finish.

			lockfree_ring_buffer_t buf( width * height );

			struct task_t {
				// funtion pointer
				// parameters
				void ( *fp )( task_t* params );
				unsigned                   x;
				unsigned                   y;
				Vec3f*                     image;
				std::vector<Sphere> const* spheres;
			};

			auto f_ptr = []( task_t* params ) {
				float xx = ( 2 * ( ( params->x + 0.5 ) * invWidth ) - 1 ) * angle * aspectratio;
				float yy = ( 1 - 2 * ( ( params->y + 0.5 ) * invHeight ) ) * angle;
				Vec3f raydir( xx, yy, -1 );
				raydir.normalize();
				params->image[ params->y * width + params->x ] =
				    trace( Vec3f( 0 ), raydir, *params->spheres, 0 );
			};

			std::vector<task_t> task_store;
			task_store.reserve( width * height );
			// build up tasks

			for ( unsigned y = 0; y < height; ++y ) {
				for ( unsigned x = 0; x < width; ++x ) {
					task_t t;
					t.fp      = f_ptr;
					t.x       = x;
					t.y       = y;
					t.image   = image;
					t.spheres = &spheres;
					task_store.emplace_back( t );
				}
			}

			for ( auto& t : task_store ) {
				buf.unsafe_initial_dynamic_push( &t );
			}

			for ( int i = 0; i != num_threads; i++ ) {
				threads.emplace_back( [ &inflight_tasks, &buf ]( std::stop_token should_stop ) {
					std::cout << "New worker thread on CPU " << sched_getcpu()
					          << std::endl;
					while ( !should_stop.stop_requested() ) {
						// execute a work unit if we could find one
						task_t* task = reinterpret_cast<task_t*>( buf.try_pop() );
						if ( task ) {
							task->fp( task );
							--inflight_tasks;
							// std::cout << inflight_tasks << std::endl;
						}
					}
				} );
			}

			// wait until number of completed tasks is width * height
			//
			// this means that the scheduling thread does not do any useful work
			// anymore apart from checking that all other work is completed.

			while ( inflight_tasks ) {
				std::this_thread::sleep_for( std::chrono::microseconds( 10 ) );
			}

			std::cout << "inflight tasks have completed" << std::endl;
			for ( auto& t : threads ) {
				t.request_stop();
			}
			// once all work units have been completed, we can join all threads
			threads.clear();

			// we implement a basic thread-based scheduler here so that we have something to
			// compare against
		}
	}

	// Save result to a PPM image (keep these flags if you compile under Windows)
	std::ofstream ofs( "./image.ppm", std::ios::out | std::ios::binary );
	ofs << "P6\n"
	    << width << " " << height << "\n255\n";
	for ( unsigned i = 0; i < width * height; ++i ) {
		ofs << ( unsigned char )( std::min( float( 1 ), image[ i ].x ) * 255 )
		    << ( unsigned char )( std::min( float( 1 ), image[ i ].y ) * 255 )
		    << ( unsigned char )( std::min( float( 1 ), image[ i ].z ) * 255 );
	}
	ofs.close();
	delete[] image;
}

//[comment]
// In the main function, we will create the scene which is composed of 5 spheres
// and 1 light (which is also a sphere). Then, once the scene description is complete
// we render that scene, by calling the render() function.
//[/comment]
int raytracer_main( int argc, char** argv ) {

	srand48( 13 );

	std::vector<Sphere> spheres;
	// position, radius, surface color, reflectivity, transparency, emission color
	spheres.push_back( Sphere( Vec3f( 0.0, -10004, -20 ), 10000, Vec3f( 0.20, 0.20, 0.20 ), 0, 0.0 ) );
	spheres.push_back( Sphere( Vec3f( 0.0, 0, -20 ), 4, Vec3f( 1.00, 0.32, 0.36 ), 1, 0.5 ) );
	spheres.push_back( Sphere( Vec3f( 5.0, -1, -15 ), 2, Vec3f( 0.90, 0.76, 0.46 ), 1, 0.0 ) );
	spheres.push_back( Sphere( Vec3f( 5.0, 0, -25 ), 3, Vec3f( 0.65, 0.77, 0.97 ), 1, 0.0 ) );
	spheres.push_back( Sphere( Vec3f( -5.5, 0, -15 ), 3, Vec3f( 0.90, 0.90, 0.90 ), 1, 0.0 ) );
	// light
	spheres.push_back( Sphere( Vec3f( 0.0, 20, -30 ), 3, Vec3f( 0.00, 0.00, 0.00 ), 0, 0.0, Vec3f( 3 ) ) );

	// argument 0 is the path to the application
	// argument 1 is the number of threads specified, if any
	int         num_threads = argc >= 2 ? atoi( argv[ 1 ] ) : 1;
	char const* choices     = argc >= 3 ? argv[ 2 ] : "01";

	render( num_threads, choices, spheres );

	return 0;
}
