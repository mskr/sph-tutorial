// Real-Time Physics Tutorials
// Brandon Pelfrey
// SPH Fluid Simulation

// Forked by Marius Kircher.
/* Devlog *                                                          *|
To understand SPH it really helped to imagine myself as a particle.
I have a history of previous and future positions.
Thinking, as well as executing code to move, takes time.
The space around me cannot easily be seen.
To know how I move, I need to know which particles influence me.
This means searching nearest neighbors.
In a fluid, neighbors are not fixed.
Therefore the search has to be done again and again.
However, in the sense of a heuristic, I can keep my neighbors from
last time, to have a starting set, from which to find neighbors faster.
Note that there is theoretically no maximum of neighbors in a radius.
An extreme of the heuristic would be when every particle stores all
other particles sorted by smallest-distance-first.
Another "short-cut" for the search was already implemented by Brandon.
It is hashing and it maps
1. positions in space
2. to cells on a grid
3. to an index in an array.
The array elements are lists of particles at that space cell.
Aside, spheres would be better than cells, because of my search radius,
but spheres are harder to map to. Also they cannot fill space efficiently.
Coming back to spatial hashing, when matching cell size with radius, I only need to consider
a few cells (e.g. 1 or 9) and ignore the rest - that's the shortcut.
An octree would do the same.
Trouble comes, when particles move and the grid must be updated.
Brandon throws away everything and pushes each particle in its cell again.
I feel that there must be a better way - keeping history.
My heuristic still needs to look at all particles, except
if I added the assumption, that far particles from last time cannot have come
near by now. In other words I impose a velocity limit.
This could be connected to the speed of sound, hmm.
To be continued...
What does the state of the art say?
https://cgl.ethz.ch/Downloads/Publications/Papers/2019/Sol19b/Sol19b.pdf
*/



#include <glm/glm.hpp>
#include <omp.h>

#include <chrono>
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <string>
#include <direct.h> // _getcwd

#include <gl-windows.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// --------------------------------------------------------------------

#define MATERIAL_DEFAULT 0
#define MATERIAL_SNOW 1
#define MATERIAL_SLIME 2

#define CURRENT_MATERIAL MATERIAL_DEFAULT

// --------------------------------------------------------------------

using namespace std::chrono;
static duration<double, std::milli> stepTime_;

// --------------------------------------------------------------------
// Between [0,1]
float rand01()
{
    return (float)rand() * (1.f / RAND_MAX);
}

// --------------------------------------------------------------------
// Between [a,b]
float randab(float a, float b)
{
    return a + (b-a)*rand01();
}

// --------------------------------------------------------------------
// Data structures are 8 byte aligned for optimal loading on 64 bit systems
__pragma(pack(push, 8))

struct Neighbor;

// The Particle structure holding all of the relevant information.
// A structure-of-arrays approach is used, i.e. stuff is grouped for cache efficiency.
struct Particles
{
    struct Position {
        glm::vec2 pos;
        float a = .0f; // used to mark neighborhood
    };
    struct Meta {

        unsigned int id; // index, valid for all data arrays

        float r, g, b; // debug color

        //glm::mat2 G; //TODO anisotropy matrix

        glm::vec2 pos_old; // for verlet?
        glm::vec2 vel;
        glm::vec2 force;
        float mass; // never used
        float rho; // density
        float rho_near; // ?
        float press;
        float press_near;
        float sigma; // linear viscosity coefficient
        float beta; // quadratic viscosity coefficient

        // current neighbors 
        // found via spatial hashing
        // cleared when particle moves
        //TODO try storing another array of all particles here,
        // sorted by distance to this particle, 
        // incrementally re-sort similar to sweep'n'prune
        Neighbor* neighbors;
        size_t neighbor_count;
    };
    Position* positions;
    Meta* meta;
    unsigned int N;
};

// A structure for holding two neighboring particles and their weighted distances
struct Neighbor
{
    unsigned int id; // index into data arrays
    float q, q2; // result and squared result of kernel estimation 1 - ( r_ij / r_max )
};

// Our collection of particles
Particles particles;

//TODO
//     load positions to GPU
//     do metaballs, other distance fields, Parzen window, ellipses with PCA...
//     problem: these methods do weighted sums over ALL particles to determine smooth contributions at each pixel

// --------------------------------------------------------------------
const float G = .02f * .25;           // Gravitational Constant for our simulation
const float spacing = 2.f;            // Spacing of particles
#if CURRENT_MATERIAL == MATERIAL_DEFAULT
const float k = spacing / 1000.0f;    // Far pressure weight
const float k_near = k * 10;          // Near pressure weight
const float rest_density = 3;         // Rest Density
#elif CURRENT_MATERIAL == MATERIAL_SLIME
const float k = spacing / 100.0f;    // Far pressure weight
const float k_near = k * 1;          // Near pressure weight
const float rest_density = 3;         // Rest Density
#elif CURRENT_MATERIAL == MATERIAL_SNOW //TODO still to bouncy...
const float k = spacing / 1000.0f;    // Far pressure weight
const float k_near = k * 10;          // Near pressure weight
const float rest_density = 10;         // Rest Density
#endif
const float r = spacing * 10.25f;      // Radius of Support
const float rsq = r * r;              // ... squared for performance stuff
const float SIM_W = 50;               // The size of the world
const float bottom = 0;               // The floor of the world

/*
Radius of support r determines the region of neighbors to be considered for smoothing.
Smoothing kernel W maps radii r_ij to weights q, so that forces are stronger when particles are nearer.
The kernel uses r to let q drop to zero after a finite support.
This helps to restrict computation to few neighbors, but note that a fixed number cannot be given.
*/

// SPH kernel function, sometimes abbreviated W
// Takes radial distance r and support radius h
// Returns influence of two points on each other
float kernel(float r, float h) {
    return 1 - (r / h);
}

// --------------------------------------------------------------------
void init( const unsigned int N )
{
    particles.N = N;
    particles.positions = (Particles::Position*)malloc(N * sizeof(Particles::Position));
    particles.meta = (Particles::Meta*)malloc(N * sizeof(Particles::Meta));

    unsigned int i = 0;

    // Initialize particles
    // We will make a block of particles with a total width of 1/4 of the screen.
    float w = SIM_W / 4;
    for( float y = bottom + w; true; y += r * 0.5f )
    {
        if (i >= N)
        {
            break;
        }
        for(float x = -w; x <= w; x += r * 0.5f )
        {
            if( i >= N )
            {
                break;
            }

            Particles::Position p;
            p.pos = glm::vec2(x, y);
            particles.positions[i] = p;

            Particles::Meta m;
            m.id = i;
            m.pos_old = p.pos + 0.001f * glm::vec2(rand01(), rand01());
            m.force = glm::vec2(0,0);
            m.sigma = 3.f;
            m.beta = 4.f;
            m.neighbors = (Neighbor*)malloc(sizeof(Neighbor));
            m.neighbor_count = 0;
            particles.meta[i] = m;

            i++;
        }
    }
}

void shutdown() {
    for (unsigned int i = 0; i < particles.N; i++)
        free(particles.meta[i].neighbors);
    free(particles.meta);
    free(particles.positions);
}

// Mouse attractor
glm::vec2 attractor(999,999);
bool attracting = false;

// --------------------------------------------------------------------
template< typename T >
class SpatialIndex
{
    const float mInvCellSize;

    // 3x3 neighborhood for 2D
    // (just edit this array to support 3D)
    const glm::ivec3 mOffsets[9] = {
        { -1, -1, 0 },{ 0, -1, 0 },{ 1, -1, 0 },
        { -1,  0, 0 },{ 0,  0, 0 },{ 1,  0, 0 },
        { -1,  1, 0 },{ 0,  1, 0 },{ 1,  1, 0 } };

public:
    typedef std::vector< T* > NeighborList;

    SpatialIndex
        (
        const unsigned int numBuckets,  // number of hash buckets
        const float cellSize           // grid cell size
        )
        : mHashMap( numBuckets )
        , mInvCellSize( 1.0f / cellSize )
    {}

    void Insert( const glm::vec3& pos, T* thing )
    {
        mHashMap[ Discretize( pos, mInvCellSize ) ].push_back( thing );
    }

    void Neighbors( const glm::vec3& pos, NeighborList& ret ) const
    {
        const glm::ivec3 ipos = Discretize( pos, mInvCellSize );
        for( const auto& offset : mOffsets )
        {
            typename HashMap::const_iterator it = mHashMap.find( offset + ipos );
            if( it != mHashMap.end() )
            {
                ret.insert( ret.end(), it->second.begin(), it->second.end() );
            }
        }
    }

    void Clear()
    {
        mHashMap.clear();
    }

private:
    // "Optimized Spatial Hashing for Collision Detection of Deformable Objects"
    // Teschner, Heidelberger, et al.
    // returns a hash between 0 and 2^32-1
    struct TeschnerHash : std::unary_function< glm::ivec3, std::size_t >
    {
        std::size_t operator()( glm::ivec3 const& pos ) const
        {
            const unsigned int p1 = 73856093;
            const unsigned int p2 = 19349663;
            const unsigned int p3 = 83492791;
            return size_t( ( pos.x * p1 ) ^ ( pos.y * p2 ) ^ ( pos.z * p3 ) );
        };
    };

    // returns the indexes of the cell pos is in, assuming a cellSize grid
    // invCellSize is the inverse of the desired cell size
    static inline glm::ivec3 Discretize( const glm::vec3& pos, const float invCellSize )
    {
        return glm::ivec3( glm::floor( pos * invCellSize ) );
    }

    // Map grid positions to dynamic list of local objects using custom hash function
    typedef std::unordered_map< glm::ivec3, NeighborList, TeschnerHash > HashMap;
    HashMap mHashMap;
};

// Hash table that can compute 1D index from 2D or 3D positions
// Template arg is the hashed object type, here particle id
// First ctor arg is hash table size
// Second ctor arg is grid cell size which determines the considered neighborhood
SpatialIndex<unsigned int> indexsp( 4093, r );

// --------------------------------------------------------------------
void step()
{
	high_resolution_clock::time_point start = high_resolution_clock::now();

    // UPDATE
    // This modified verlet integrator has dt = 1 and calculates the velocity
    // For later use in the simulation.
#pragma omp parallel for
    for( int i = 0; i < (int)particles.N; ++i )
    {
        // Apply the currently accumulated forces
        particles.positions[i].pos += particles.meta[i].force;

        // Restart the forces with gravity only. We'll add the rest later.
        particles.meta[i].force = glm::vec2( 0.0f, -::G );

        // Calculate the velocity for later.
        particles.meta[i].vel = particles.positions[i].pos - particles.meta[i].pos_old;

        // If the velocity is really high, we're going to cheat and cap it.
        // This will not damp all motion. It's not physically-based at all. Just
        // a little bit of a hack.
        const float max_vel = 2.0f;
        const float vel_mag = glm::dot( particles.meta[i].vel, particles.meta[i].vel );
        // If the velocity is greater than the max velocity, then cut it in half.
        if( vel_mag > max_vel * max_vel )
        {
            particles.meta[i].vel *= .5f;
        }

        // Normal verlet stuff
        particles.meta[i].pos_old = particles.positions[i].pos;
        particles.positions[i].pos += particles.meta[i].vel;

        // If the Particle is outside the bounds of the world, then
        // Make a little spring force to push it back in.
        if( particles.positions[i].pos.x < -SIM_W ) particles.meta[i].force.x -= ( particles.positions[i].pos.x - -SIM_W ) / 8;
        if( particles.positions[i].pos.x >  SIM_W ) particles.meta[i].force.x -= ( particles.positions[i].pos.x - SIM_W ) / 8;
        if( particles.positions[i].pos.y < bottom ) particles.meta[i].force.y -= ( particles.positions[i].pos.y - bottom ) / 8;
        //if( particles.positions[i].pos.y > SIM_W * 2 ) particles.meta[i].force.y -= ( particles.positions[i].pos.y - SIM_W * 2 ) / 8;

        // Handle the mouse attractor.
        // It's a simple spring based attraction to where the mouse is.
        const float attr_dist2 = glm::dot( particles.positions[i].pos - attractor, particles.positions[i].pos - attractor );
        const float attr_l = SIM_W / 4;
        if( attracting )
        {
            if( attr_dist2 < attr_l * attr_l )
            {
                particles.meta[i].force -= ( particles.positions[i].pos - attractor ) / 256.0f;
            }
        }

        // Reset the nessecary items.
        particles.meta[i].rho = 0;
        particles.meta[i].rho_near = 0;
        particles.meta[i].neighbor_count = 0;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////

    // SPATIAL INDEX

    // Throw away all previous neighbor information
    indexsp.Clear();
    //TODO investigate incremental update and if applicable measure perf gain

    // Sequential iteration since the hash map is not thread-safe
    for (unsigned int i = 0; i < particles.N; ++i)
    {
        // Insert includes 
        // 1. discretization (3x div by grid step),
        // 2. hash function evaluation (ivec3 to int) and 
        // 3. list realloc
        indexsp.Insert( glm::vec3( particles.positions[i].pos, 0.0f ), &particles.meta[i].id );
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////

    // DENSITY
    // Calculate the density by basically making a weighted sum
    // of the distances of neighboring particles within the radius of support (r)
#pragma omp parallel for
    for( int i = 0; i < (int)particles.N; ++i )
    {
        particles.meta[i].rho = 0;
        particles.meta[i].rho_near = 0;

        // We will sum up the 'near' and 'far' densities.
        float d = 0;
        float dn = 0;

        std::vector<unsigned int*> neighIds;
        neighIds.reserve( 64 );
        indexsp.Neighbors( glm::vec3( particles.positions[i].pos, 0.0f ), neighIds );
        for( int j = 0; j < (int)neighIds.size(); ++j )
        {
            if( *neighIds[j] == particles.meta[i].id )
            {
                // do not calculate an interaction for a Particle with itself!
                continue;
            }

            // The vector seperating the two particles
            const glm::vec2 rij = particles.positions[*neighIds[j]].pos - particles.positions[i].pos;

            // Along with the squared distance between
            const float rij_len2 = glm::dot( rij, rij );

            // If they're within the radius of support ...
            if( rij_len2 < rsq )
            {
                // Get the actual distance from the squared distance.
                float rij_len = sqrt( rij_len2 );

                // And calculated the weighted distance values
                const float q = kernel(rij_len, r);
                const float q2 = q * q;
                const float q3 = q2 * q;

                d += q2;
                dn += q3;

                // Set up the Neighbor list for faster access later.
                Neighbor n;
                n.id = *neighIds[j];
                n.q = q;
                n.q2 = q2;
                particles.meta[i].neighbors = (Neighbor*)realloc(
                    particles.meta[i].neighbors, 
                    (particles.meta[i].neighbor_count + 1) * sizeof(Neighbor));
                particles.meta[i].neighbors[particles.meta[i].neighbor_count] = n;
                particles.meta[i].neighbor_count++;
            }
        }

        particles.meta[i].rho += d;
        particles.meta[i].rho_near += dn;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////////

    // PRESSURE
    // Make the simple pressure calculation from the equation of state.
    // Compressibility issues come into play here.
    // Approaches:
    // Divergence-free SPH: compute k based on individual neighborhoods
    // PBF: position based constraint equation
    // IISPH: implicit ISPH
    // WCSPH: weakly compressible
    // ISPH: icompressibile by doing "pressure projection"
#pragma omp parallel for
    for( int i = 0; i < (int)particles.N; ++i )
    {
        particles.meta[i].press = k * ( particles.meta[i].rho - rest_density );
        particles.meta[i].press_near = k_near * particles.meta[i].rho_near;
    }

    // PRESSURE FORCE
    // We will force particles in or out from their neighbors
    // based on their difference from the rest density.
#pragma omp parallel for
    for( int i = 0; i < (int)particles.N; ++i )
    {
        // For each of the neighbors
        glm::vec2 dX( 0 );
        for( size_t j = 0; j < particles.meta[i].neighbor_count; j++ )
        {
            const Neighbor& n_j = particles.meta[i].neighbors[j];

            // The vector from Particle i to Particle j
            const glm::vec2 rij = particles.positions[n_j.id].pos - particles.positions[i].pos;

            // calculate the force from the pressures calculated above
            const float dm
                = n_j.q * ( particles.meta[i].press + particles.meta[n_j.id].press )
				+ n_j.q2 * ( particles.meta[i].press_near + particles.meta[n_j.id].press_near );

            // Get the direction of the force
            const glm::vec2 D = glm::normalize( rij ) * dm;
            dX += D;
        }

        particles.meta[i].force -= dX;
    }

    // VISCOSITY
    // This simulation actually may look okay if you don't compute
    // the viscosity section. The effects of numerical damping and
    // surface tension will give a smooth appearance on their own.
    // Try it.
#pragma omp parallel for
    for( int i = 0; i < (int)particles.N; ++i )
    {
        // We'll let the color be determined by
        // ... x-velocity for the red component
        // ... y-velocity for the green-component
        // ... pressure for the blue component
        particles.meta[i].r = 0.3f + (20 * fabs(particles.meta[i].vel.x) );
        particles.meta[i].g = 0.3f + (20 * fabs(particles.meta[i].vel.y) );
        particles.meta[i].b = 0.3f + (0.1f * particles.meta[i].rho );

        // For each of that particles neighbors
        for (size_t j = 0; j < particles.meta[i].neighbor_count; j++)
        {
            const Neighbor& n_j = particles.meta[i].neighbors[j];

            const glm::vec2 rij = particles.positions[n_j.id].pos - particles.positions[i].pos;
            const float l = glm::length( rij );
            const float q = l / r;

            const glm::vec2 rijn = ( rij / l );
            // Get the projection of the velocities onto the vector between them.
            const float u = glm::dot( particles.meta[i].vel - particles.meta[n_j.id].vel, rijn );
            if( u > 0 )
            {
                // Calculate the viscosity impulse between the two particles
                // based on the quadratic function of projected length.
                const glm::vec2 I
                    = ( 1 - q )
                    * (particles.meta[n_j.id].sigma * u + particles.meta[n_j.id].beta * u * u )
                    * rijn;

                // Apply the impulses on the current particle
                particles.meta[i].vel -= I * 0.5f;
            }
        }
    }

	stepTime_ = high_resolution_clock::now() - start;
}


// --------------------------------------------------------------------
int main(int argc, char** argv)
{
#if 0
    const int steps = 3000;
    std::cout << "--------------------------------" << std::endl;
    std::cout << "Number of steps: " << steps << std::endl;
    for (unsigned int size = 10; size <= 13; ++size)
    {
        const unsigned int count = (1 << size);
        std::cout << "Number of particles: " << count << std::endl;

        init(count);

        const auto beg = std::chrono::high_resolution_clock::now();
        for (unsigned int i = 0; i < steps; ++i)
        {
            step();
        }
        const auto end = std::chrono::high_resolution_clock::now();

        const auto duration(end - beg);
        std::cout << "Elapsed time: " << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() << " milliseconds" << std::endl;
        std::cout << "Microseconds per step: " << std::chrono::duration_cast<std::chrono::microseconds>(duration).count() / (double)steps << std::endl;
        std::cout << std::endl;
    }

    return 0;
#else

    //TODO Sand, soil, snow (strong cohesion, high rest density, weak spring forces)

    //TODO Try solid material with very strong springs forces.
    // Then implement control to transition between fluid and solid phases.
    // (Sculpting with fluid to solid jamming)
    // (https://graphics.ethz.ch/~sobarbar/papers/Sol07b/Sol07b.pdf)

    //TODO Timeline seeking (use ImgGUI)




    //TODO after curvature flow: attempt to draw a line around silhouette, i.e. level set boundary

    init(200);

    uint64_t gdiContext, glContext;
    createGLContexts(&gdiContext, &glContext);

    char cwd[256];
    _getcwd(cwd, 256);
    int width, height, bpp;
    unsigned char* rgb = stbi_load((std::string(cwd) + "/../img/cobble.jpg").c_str(), &width, &height, &bpp, 3);
    assert(rgb);
    unsigned int img = 0; createGLImage(width, height, &img, rgb, 3);
    stbi_image_free(rgb);

    float proj[4][4]{ { 1.f / SIM_W, 0, 0, 0 }, { 0, 1.f / SIM_W, 0, -1.f }, { 0, 0, 1.f, 0 }, { 0, 0, 0, 1.f } };
    pushGLView(&proj[0][0]);

    GLVertexHandle verts;
    createGLPoints2D(particles.N * sizeof(Particles::Position), &verts, particles.positions);
    /*
    Generate quads from particle positions (tri strip with 4 vertices)
    Each quad contains
     - local uv coordinate system to render spherical normals and depth
     - list of neighbor indices (into position buffer that is always on GPU)
       (similar to connecting vertices to bones in skeletal animation)
     (- non-uniform xy scaling based on principal components)
     Splatting: http://www.cs.rug.nl/~roe/courses/acg/rendering
    */

    pushGLView(); createGLQuad();
    pushGLView(); createGLQuad();


    float curvatureFlowFactor = .001f; ;

    std::vector<unsigned int*> lastNeighIds;

    openGLWindowAndREPL();

    unsigned int mouse[2]; bool mouseDown; char pressedKey;

    while (processWindowsMessage(mouse, &mouseDown, &pressedKey)) {

        unsigned int window[2]; getGLWindowSize(window);

        //printf("mouse=%d,%d   mouseDown=%d   pressedKey=%c\n\n", mouse[0], mouse[1], mouseDown, pressedKey);
        {
            float relx = (float)((int)mouse[0] - (int)window[0] / 2) / (int)window[0];
            float rely = -(float)((int)mouse[1] - (int)window[1]) / (int)window[1];
            const auto projMouse = glm::vec2(relx*SIM_W * 2, rely*SIM_W * 2);
            if (attracting = mouseDown) {
                attractor = projMouse;
            }
            else {
                attractor = glm::vec2(SIM_W * 99, SIM_W * 99);

                // mark neighborhood
                std::vector<unsigned int*> neighIds;
                neighIds.reserve(64);
                indexsp.Neighbors(glm::vec3(projMouse, 0.0f), neighIds);
                for (const auto i : lastNeighIds) particles.positions[*i].a = 0.f;
                for (const auto i : neighIds) particles.positions[*i].a = 1.f;
                lastNeighIds = neighIds;
            }
            updateGLLightSource(relx, rely, .5f);
        }

        runGLShader(GLShaderParam{ "curvatureFlowFactor", &curvatureFlowFactor, .0f, .01f });

        step();

        updateGLVertexData(verts, particles.N * sizeof(Particles::Position), particles.positions);

        swapGLBuffers(60);

    }
    closeGLWindowAndREPL();

    shutdown();
    return 0;
#endif
}