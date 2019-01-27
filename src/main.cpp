// Real-Time Physics Tutorials
// Brandon Pelfrey
// SPH Fluid Simulation

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
// A structure for holding two neighboring particles and their weighted distances
struct Particle;
struct Neighbor
{
    Particle* j;
    float q, q2;
};

// The Particle structure holding all of the relevant information.
struct Particle
{
    glm::vec2 pos;
    float r, g, b;

    glm::vec2 pos_old; // for verlet?
    glm::vec2 vel;
    glm::vec2 force;
    float mass;
    float rho; // density
    float rho_near; // ?
    float press;
    float press_near;
    float sigma; // ?
    float beta; // ?
    std::vector< Neighbor > neighbors;
};

// Our collection of particles
std::vector< Particle > particles;

// --------------------------------------------------------------------
const float G = .0f;// .02f * .25;           // Gravitational Constant for our simulation
const float spacing = 2.f;            // Spacing of particles
const float k = spacing / 1000.0f;    // Far pressure weight
const float k_near = k * 10;          // Near pressure weight
const float rest_density = 3;         // Rest Density
const float r = spacing * 1.25f;      // Radius of Support
const float rsq = r * r;              // ... squared for performance stuff
const float SIM_W = 50;               // The size of the world
const float bottom = 0;               // The floor of the world

// --------------------------------------------------------------------
void init( const unsigned int N )
{
    // Initialize particles
    // We will make a block of particles with a total width of 1/4 of the screen.
    float w = SIM_W / 4;
    for( float y = bottom + w; y <= w+w*2.f; y += r * 0.5f )
    {
        for(float x = -w; x <= w; x += r * 0.5f )
        {
            if( particles.size() > N )
            {
                break;
            }

            Particle p;
            p.pos = glm::vec2(x, y);
            p.pos_old = p.pos + 0.001f * glm::vec2(rand01(), rand01());
            p.force = glm::vec2(0,0);
            p.sigma = 3.f;
            p.beta = 4.f;
            particles.push_back(p);
        }
    }
}

// Mouse attractor
glm::vec2 attractor(999,999);
bool attracting = false;

// --------------------------------------------------------------------
template< typename T >
class SpatialIndex
{
public:
    typedef std::vector< T* > NeighborList;

    SpatialIndex
        (
        const unsigned int numBuckets,  // number of hash buckets
        const float cellSize,           // grid cell size
        const bool twoDeeNeighborhood   // true == 3x3 neighborhood, false == 3x3x3
        )
        : mHashMap( numBuckets )
        , mInvCellSize( 1.0f / cellSize )
    {
        // initialize neighbor offsets
        for( int i = -1; i <= 1; i++ )
            for( int j = -1; j <= 1; j++ )
                if( twoDeeNeighborhood )
                    mOffsets.push_back( glm::ivec3( i, j, 0 ) );
                else
                    for( int k = -1; k <= 1; k++ )
                        mOffsets.push_back( glm::ivec3( i, j, k ) );
    }

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

    typedef std::unordered_map< glm::ivec3, NeighborList, TeschnerHash > HashMap;
    HashMap mHashMap;

    std::vector< glm::ivec3 > mOffsets;

    const float mInvCellSize;
};

typedef SpatialIndex< Particle > IndexType;
IndexType indexsp( 4093, r, true );

// --------------------------------------------------------------------
void step()
{
	high_resolution_clock::time_point start = high_resolution_clock::now();

    // UPDATE
    // This modified verlet integrator has dt = 1 and calculates the velocity
    // For later use in the simulation.
#pragma omp parallel for
    for( int i = 0; i < (int)particles.size(); ++i )
    {
        // Apply the currently accumulated forces
        particles[i].pos += particles[i].force;

        // Restart the forces with gravity only. We'll add the rest later.
        particles[i].force = glm::vec2( 0.0f, -::G );

        // Calculate the velocity for later.
        particles[i].vel = particles[i].pos - particles[i].pos_old;

        // If the velocity is really high, we're going to cheat and cap it.
        // This will not damp all motion. It's not physically-based at all. Just
        // a little bit of a hack.
        const float max_vel = 2.0f;
        const float vel_mag = glm::dot( particles[i].vel, particles[i].vel );
        // If the velocity is greater than the max velocity, then cut it in half.
        if( vel_mag > max_vel * max_vel )
        {
            particles[i].vel *= .5f;
        }

        // Normal verlet stuff
        particles[i].pos_old = particles[i].pos;
        particles[i].pos += particles[i].vel;

        // If the Particle is outside the bounds of the world, then
        // Make a little spring force to push it back in.
        if( particles[i].pos.x < -SIM_W ) particles[i].force.x -= ( particles[i].pos.x - -SIM_W ) / 8;
        if( particles[i].pos.x >  SIM_W ) particles[i].force.x -= ( particles[i].pos.x - SIM_W ) / 8;
        if( particles[i].pos.y < bottom ) particles[i].force.y -= ( particles[i].pos.y - bottom ) / 8;
        //if( particles[i].pos.y > SIM_W * 2 ) particles[i].force.y -= ( particles[i].pos.y - SIM_W * 2 ) / 8;

        // Handle the mouse attractor.
        // It's a simple spring based attraction to where the mouse is.
        const float attr_dist2 = glm::dot( particles[i].pos - attractor, particles[i].pos - attractor );
        const float attr_l = SIM_W / 4;
        if( attracting )
        {
            if( attr_dist2 < attr_l * attr_l )
            {
                particles[i].force -= ( particles[i].pos - attractor ) / 256.0f;
            }
        }

        // Reset the nessecary items.
        particles[i].rho = 0;
        particles[i].rho_near = 0;
        particles[i].neighbors.clear();
    }

    // update spatial index
    indexsp.Clear();
    for( auto& particle : particles )
    {
        indexsp.Insert( glm::vec3( particle.pos, 0.0f ), &particle );
    }

    // DENSITY
    // Calculate the density by basically making a weighted sum
    // of the distances of neighboring particles within the radius of support (r)
#pragma omp parallel for
    for( int i = 0; i < (int)particles.size(); ++i )
    {
        particles[i].rho = 0;
        particles[i].rho_near = 0;

        // We will sum up the 'near' and 'far' densities.
        float d = 0;
        float dn = 0;

        IndexType::NeighborList neigh;
        neigh.reserve( 64 );
        indexsp.Neighbors( glm::vec3( particles[i].pos, 0.0f ), neigh );
        for( int j = 0; j < (int)neigh.size(); ++j )
        {
            if( neigh[j] == &particles[i] )
            {
                // do not calculate an interaction for a Particle with itself!
                continue;
            }

            // The vector seperating the two particles
            const glm::vec2 rij = neigh[j]->pos - particles[i].pos;

            // Along with the squared distance between
            const float rij_len2 = glm::dot( rij, rij );

            // If they're within the radius of support ...
            if( rij_len2 < rsq )
            {
                // Get the actual distance from the squared distance.
                float rij_len = sqrt( rij_len2 );

                // And calculated the weighted distance values
                const float q = 1 - ( rij_len / r );
                const float q2 = q * q;
                const float q3 = q2 * q;

                d += q2;
                dn += q3;

                // Set up the Neighbor list for faster access later.
                Neighbor n;
                n.j = neigh[j];
                n.q = q;
                n.q2 = q2;
                particles[i].neighbors.push_back(n);
            }
        }

        particles[i].rho += d;
        particles[i].rho_near += dn;
    }

    // PRESSURE
    // Make the simple pressure calculation from the equation of state.
#pragma omp parallel for
    for( int i = 0; i < (int)particles.size(); ++i )
    {
        particles[i].press = k * ( particles[i].rho - rest_density );
        particles[i].press_near = k_near * particles[i].rho_near;
    }

    // PRESSURE FORCE
    // We will force particles in or out from their neighbors
    // based on their difference from the rest density.
#pragma omp parallel for
    for( int i = 0; i < (int)particles.size(); ++i )
    {
        // For each of the neighbors
        glm::vec2 dX( 0 );
        for( const Neighbor& n : particles[i].neighbors )
        {
            // The vector from Particle i to Particle j
            const glm::vec2 rij = (*n.j).pos - particles[i].pos;

            // calculate the force from the pressures calculated above
            const float dm
                = n.q * ( particles[i].press + (*n.j).press )
				+ n.q2 * ( particles[i].press_near + (*n.j).press_near );

            // Get the direction of the force
            const glm::vec2 D = glm::normalize( rij ) * dm;
            dX += D;
        }

        particles[i].force -= dX;
    }

    // VISCOSITY
    // This simulation actually may look okay if you don't compute
    // the viscosity section. The effects of numerical damping and
    // surface tension will give a smooth appearance on their own.
    // Try it.
#pragma omp parallel for
    for( int i = 0; i < (int)particles.size(); ++i )
    {
        // We'll let the color be determined by
        // ... x-velocity for the red component
        // ... y-velocity for the green-component
        // ... pressure for the blue component
        particles[i].r = 0.3f + (20 * fabs(particles[i].vel.x) );
        particles[i].g = 0.3f + (20 * fabs(particles[i].vel.y) );
        particles[i].b = 0.3f + (0.1f * particles[i].rho );

        // For each of that particles neighbors
        for( const Neighbor& n : particles[i].neighbors )
        {
            const glm::vec2 rij = (*n.j).pos - particles[i].pos;
            const float l = glm::length( rij );
            const float q = l / r;

            const glm::vec2 rijn = ( rij / l );
            // Get the projection of the velocities onto the vector between them.
            const float u = glm::dot( particles[i].vel - (*n.j).vel, rijn );
            if( u > 0 )
            {
                // Calculate the viscosity impulse between the two particles
                // based on the quadratic function of projected length.
                const glm::vec2 I
                    = ( 1 - q )
                    * ( (*n.j).sigma * u + (*n.j).beta * u * u )
                    * rijn;

                // Apply the impulses on the current particle
                particles[i].vel -= I * 0.5f;
            }
        }
    }

	stepTime_ = high_resolution_clock::now() - start;
}


// --------------------------------------------------------------------
int main( int argc, char** argv )
{
#if 0
    const int steps = 3000;
    std::cout << "--------------------------------" << std::endl;
    std::cout  << "Number of steps: " << steps << std::endl;
    for( unsigned int size = 10; size <= 13; ++size )
    {
        const unsigned int count = ( 1 << size );
        std::cout << "Number of particles: " << count << std::endl;

        init( count );

        const auto beg = std::chrono::high_resolution_clock::now();
        for( unsigned int i = 0; i < steps; ++i )
        {
            step();
        }
        const auto end = std::chrono::high_resolution_clock::now();

        const auto duration( end - beg );
        std::cout << "Elapsed time: " << std::chrono::duration_cast< std::chrono::milliseconds >( duration ).count() << " milliseconds" << std::endl;
        std::cout << "Microseconds per step: " << std::chrono::duration_cast< std::chrono::microseconds >( duration ).count() / (double)steps << std::endl;
        std::cout << std::endl;
    }

    return 0;
#else
    init( 2048 );

	uint64_t gdiContext, glContext;
	createGLContexts(&gdiContext, &glContext);

	char cwd[256];
	_getcwd(cwd, 256);
	int width, height, bpp;
	unsigned char* rgb = stbi_load((std::string(cwd) + "/../img/cobble.jpg").c_str(), &width, &height, &bpp, 3);
	assert(rgb);
	unsigned int img = 0; createGLImage(width, height, &img, rgb, 3);
	stbi_image_free(rgb);

	pushGLView(/*scale=*/1.0f / SIM_W, 1.0f / SIM_W, /*translation=*/0, -1.0f);

	GLVertexHandle verts;
	createGLPoints2D(particles.size() * sizeof(Particle), &verts, particles.data(), sizeof(Particle));

	pushGLView();

	createGLQuad();

	openGLWindowAndREPL();
	unsigned int mouse[2]; bool mouseDown; char pressedKey;
	while (processWindowsMessage(mouse, &mouseDown, &pressedKey)) {
		
		unsigned int window[2]; getGLWindowSize(window);

		//printf("mouse=%d,%d   mouseDown=%d   pressedKey=%c\n\n", mouse[0], mouse[1], mouseDown, pressedKey);
		if (attracting = mouseDown) {
			float relx = (float)((int)mouse[0] - (int)window[0] / 2) / (int)window[0];
			float rely = -(float)((int)mouse[1] - (int)window[1]) / (int)window[1];
			attractor = glm::vec2(relx*SIM_W * 2, rely*SIM_W * 2);
		} else {
			attractor = glm::vec2(SIM_W * 99, SIM_W * 99);
		}

		runGLShader();

		step();

		updateGLVertexData(verts, particles.size() * sizeof(Particle), particles.data());

		swapGLBuffers(60);

	}
	closeGLWindowAndREPL();
    return 0;
#endif
}
