sph-tutorial
============

Experimenting with performance evaluation and improvements to Brandon Pelfrey's [smoothed-particle hydrodynamics (SPH)](https://en.wikipedia.org/wiki/Smoothed-particle_hydrodynamics) fluid simulation tutorial:

* [Real-time Physics 101: Let’s Talk Particles](https://web.archive.org/web/20090530024753/http://blog.brandonpelfrey.com/?p=58)
* [Real-time Physics 102: Springboard into Constraints](https://web.archive.org/web/20090531100829/http://blog.brandonpelfrey.com/?p=242)
* [Real-Time Physics 103: Fluid Simulation in Games](https://web.archive.org/web/20090722233436/http://blog.brandonpelfrey.com/?p=303)

##### Requirements

* CMake >= 3.7.2
* C++11 compiler
* OpenGL support
* OpenMP support

##### Controls

* Q/Escape: Exit
* Space: Add more particles
* Mouse button: Attract nearby particles
* +/-: Increase/decrease the number of simulation steps per frame

You can use the [`OMP_NUM_THREADS` environment variable](https://gcc.gnu.org/onlinedocs/libgomp/OMP_005fNUM_005fTHREADS.html#OMP_005fNUM_005fTHREADS) to limit the number of threads used by OpenMP.

##### Building

The repo is self-contained, so you should be able to clone and build without anything more than CMake and a compiler:

    git clone https://github.com/genpfault/sph-tutorial.git
    cd sph-tutorial
    git submodule update --init --recursive
    mkdir build
    cd build
    cmake ../ -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build .


##### Devlog by mskr

I wanted to develop my own particle-based interactive fluid simulation in order to try building new creative experiences based on formable, mixable and flowing materials with unified physics. I also try to turn this vague and broad goal somehow into a workable topic for my master thesis in media informatics. So I forked this repo to quickly get started.

I was positively surprised how easy it was to build and, even better, to actually see beautiful animations. Since I can be motivated only by graphical results, I began plugging in my GL renderer with shaders (which works only in Windows, because it uses the console with native commands to turn it into a kind of shader REPL). It took a bit too much effort, considering that it is maybe not even such a nice tool and similar things sure do exist. Anyway, next I wanted to see, if I can improve the surface reconstruction and rendering of the fluid. Popular methods seem to be Curvature Flow and Ellipsoid Point Splatting, of which I added the original papers to the repo.

Meanwhile, master thesis took a different turn, I am now doing blood flow simulation. I have to first review existing finite element solutions. But it may lead me back here later.

A parallel devlog is at the top of [src/main.cpp](src/main.cpp).
