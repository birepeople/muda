
#include <catch2/catch.hpp>
#include <muda/muda.h>
#include <muda/container.h>
#include "../example_common.h"
#define _USE_MATH_DEFINES
#include <math.h>
#include <filesystem>
using namespace muda;

using Vector2 = Eigen::Vector2f;

// all const data in simulation are stored in this struct
struct ConstData
{
    // "Particle-Based Fluid Simulation for Interactive Applications" by Mueller et al.
    // solver parameters
    const Vector2 G = Vector2(0.f, -10.f);  // external (gravitational) forces
    const float   REST_DENS = 300.f;        // rest density
    const float   GAS_CONST = 2000.f;       // const for equation of state
    const float   H         = 16.f;         // kernel radius
    const float   HSQ       = H * H;        // radius^2 for optimization
    const float   MASS      = 2.5f;   // assume all particles have the same mass
    const float   VISC      = 200.f;  // viscosity constant
    const float   DT        = 0.0007f;  // integration timestep

    // smoothing kernels defined in Mueller and their gradients
    // adapted to 2D per "SPH Based Shallow Water Simulation" by Solenthaler et al.
    const float POLY6      = 4.f / (M_PI * pow(H, 8.f));
    const float SPIKY_GRAD = -10.f / (M_PI * pow(H, 5.f));
    const float VISC_LAP   = 40.f / (M_PI * pow(H, 5.f));

    // simulation parameters
    const float EPS           = H;  // boundary epsilon
    const float BOUND_DAMPING = -0.5f;

    // interaction
    // const int MAX_PARTICLES   = 2500;
    const int DAM_PARTICLES   = 1000;
    //const int BLOCK_PARTICLES = 250;

    // rendering projection parameters
    const int    WINDOW_WIDTH  = 800;
    const int    WINDOW_HEIGHT = 600;
    const double VIEW_WIDTH    = 1.5 * 800.f;
    const double VIEW_HEIGHT   = 1.5 * 600.f;
} const CONST_DATA;

// particle data structure
// stores position, velocity, and force for integration
// stores density (rho) and pressure values for SPH
struct Particle
{
    Particle(float _x, float _y, int id)
        : x(_x, _y)
        , v(0.f, 0.f)
        , f(0.f, 0.f)
        , rho(0)
        , p(0.f)
        , id(id)
    {
    }
    Particle()
        : x(0, 0)
        , v(0.f, 0.f)
        , f(0.f, 0.f)
        , rho(0)
        , p(0.f)
        , id(-1)
    {
    }

    Vector2 x, v, f;
    float   rho, p;
    int     id;

    void ToCSV(std::ostream& o) const
    {
        // expand to 3d for latter visualization
        // clang-format off
        o << x(0) << "," << x(1) << "," << 0.0f << ","  
          << v(0) << "," << v(1) << "," << 0.0f << "," 
          << f(0) << "," << f(1) << "," << 0.0f << ","
          << rho << "," << p << "," << id << std::endl;
        // clang-format on
    }

    // create header for csv file
    static void CSVHeader(std::ostream& o)
    {
        o << "x[0],x[1],x[2],v[0],v[1],v[2],f[0],f[1],f[2],rho,p,id" << std::endl;
    }
};

constexpr int BLOCK_DIM = 128;

class SPHSolver
{
    device_vector<Particle> particles;
    cudaStream_t            stream;

  public:
    SPHSolver(cudaStream_t stream = nullptr)
        : stream(stream)
    {
    }

    void SetParticles(const host_vector<Particle>& p) { particles = p; }

    void Solve()
    {
        ComputeDensityPressure();
        ComputeForces();
        Integrate();
    }

    void GetParticles(host_vector<Particle>& p)
    {
        launch::wait_device();
        // copy the particles from device to host
        p = particles;
    }

    void Integrate()
    {
        // using dynamic grid size to cover all the particles
        parallel_for(BLOCK_DIM, 0, stream)
            .apply(particles.size(),
                   [BOUND_DAMPING = CONST_DATA.BOUND_DAMPING,
                    EPS           = CONST_DATA.EPS,
                    VIEW_WIDTH    = CONST_DATA.VIEW_WIDTH,
                    VIEW_HEIGHT   = CONST_DATA.VIEW_HEIGHT,
                    DT            = CONST_DATA.DT,
                    particles = make_viewer(particles)] __device__(int i) mutable
                   {
                       auto& p = particles(i);
                       // forward Euler integration
                       p.v += DT * p.f / p.rho;
                       p.x += DT * p.v;

                       // enforce boundary conditions
                       if(p.x(0) - EPS < 0.f)
                       {
                           p.v(0) *= BOUND_DAMPING;
                           p.x(0) = EPS;
                       }
                       if(p.x(0) + EPS > VIEW_WIDTH)
                       {
                           p.v(0) *= BOUND_DAMPING;
                           p.x(0) = VIEW_WIDTH - EPS;
                       }
                       if(p.x(1) - EPS < 0.f)
                       {
                           p.v(1) *= BOUND_DAMPING;
                           p.x(1) = EPS;
                       }
                       if(p.x(1) + EPS > VIEW_HEIGHT)
                       {
                           p.v(1) *= BOUND_DAMPING;
                           p.x(1) = VIEW_HEIGHT - EPS;
                       }
                   });
    }

    void ComputeForces()
    {
        // using dynamic grid size to cover all the particles
        parallel_for(BLOCK_DIM, 0, stream)
            .apply(particles.size(),
                   [H          = CONST_DATA.H,
                    MASS       = CONST_DATA.MASS,
                    SPIKY_GRAD = CONST_DATA.SPIKY_GRAD,
                    VISC       = CONST_DATA.VISC,
                    VISC_LAP   = CONST_DATA.VISC_LAP,
                    G          = CONST_DATA.G,
                    particles = make_viewer(particles)] __device__(int i) mutable
                   {
                       auto&   pi = particles(i);
                       Vector2 fpress(0.f, 0.f);
                       Vector2 fvisc(0.f, 0.f);
                       for(int j = 0; j < particles.dim(); ++j)
                       {
                           auto& pj = particles(j);
                           if(pi.id == pj.id)
                           {
                               continue;
                           }

                           Vector2 rij = pj.x - pi.x;
                           float   r   = rij.norm();

                           if(r < H)
                           {
                               // compute pressure force contribution
                               fpress += -rij.normalized() * MASS * (pi.p + pj.p)
                                         / (2.f * pj.rho) * SPIKY_GRAD * pow(H - r, 3.f);
                               // compute viscosity force contribution
                               fvisc += VISC * MASS * (pj.v - pi.v) / pj.rho
                                        * VISC_LAP * (H - r);
                           }
                       }
                       Vector2 fgrav = G * MASS / pi.rho;
                       pi.f          = fpress + fvisc + fgrav;
                   });
    }

    void ComputeDensityPressure()
    {
        // using dynamic grid size to cover all the particles
        parallel_for(BLOCK_DIM, 0, stream)
            .apply(particles.size(),
                   [HSQ       = CONST_DATA.HSQ,
                    MASS      = CONST_DATA.MASS,
                    POLY6     = CONST_DATA.POLY6,
                    GAS_CONST = CONST_DATA.GAS_CONST,
                    REST_DENS = CONST_DATA.REST_DENS,
                    particles = make_viewer(particles)] __device__(int i) mutable
                   {
                       auto& pi = particles(i);
                       pi.rho   = 0.f;
                       for(int j = 0; j < particles.dim(); ++j)
                       {
                           auto&   pj  = particles(j);
                           Vector2 rij = pj.x - pi.x;
                           float   r2  = rij.squaredNorm();

                           if(r2 < HSQ)
                           {
                               // this computation is symmetric
                               pi.rho += MASS * POLY6 * pow(HSQ - r2, 3.f);
                           }
                       }
                       pi.p = GAS_CONST * (pi.rho - REST_DENS);
                   });
    }
};

void InitSPH(host_vector<Particle>& particles)
{
    int i = 0;
    for(float y = CONST_DATA.EPS; y < CONST_DATA.VIEW_HEIGHT - CONST_DATA.EPS * 2.f;
        y += CONST_DATA.H)
    {
        for(float x = CONST_DATA.VIEW_WIDTH / 4; x <= CONST_DATA.VIEW_WIDTH / 2;
            x += CONST_DATA.H)
        {
            if(particles.size() < CONST_DATA.DAM_PARTICLES)
            {
                float jitter =
                    static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
                particles.push_back(Particle(x + jitter, y, i++));
            }
            else
            {
                return;
            }
        }
    }
}

void ExportParticlesToCSV(const std::string& folder, int idx, host_vector<Particle>& particles)
{
    std::ofstream f;
    f.open(folder + "/" + std::to_string(idx) + ".csv");
    Particle::CSVHeader(f);
    for(const auto& p : particles)
        p.ToCSV(f);
    f.close();
}

void MakeProgress(float progress, int bar)
{
    // make a progress bar
    int         w        = std::round(progress * bar);
    std::string done(w, '>');
    std::string undone(bar - w, '=');
    std::cout << "[" << done << undone << "]\r";
}

void sph2d()
{
    example_desc(
        "a simple 2d Smoothed Particle Hydrodynamics exmaple.\n"
        "ref: https://lucasschuermann.com/writing/implementing-sph-in-2d\n");

    // create particles on host
    host_vector<Particle> particles;
    particles.reserve(CONST_DATA.DAM_PARTICLES);
    // generate particles randomly
    InitSPH(particles);
    std::cout << "initializing dam break with " << CONST_DATA.DAM_PARTICLES
              << " particles" << std::endl;

    std::cout << "delta time =" << CONST_DATA.DT << std::endl;
    std::cout << "particle count =" << CONST_DATA.DAM_PARTICLES << std::endl;

    // create a stream (RAII style, no need to destroy manually)
    stream s;

    SPHSolver solver(s);
    // set the particles in solver
    solver.SetParticles(particles);

    // create a folder for frame data output
    std::filesystem::path folder("sph2d/");
    if(!std::filesystem::exists(folder))
        std::filesystem::create_directory(folder);

    auto abspath = std::filesystem::absolute(folder);
    std::cout << "solve frames to folder: " << abspath << std::endl;

    int bar    = 77;
    int nframe = 1000;
    for(int i = 0; i < nframe; i++)
    {
        solver.Solve();
        solver.GetParticles(particles);
        MakeProgress((i + 1.0f) / nframe, bar);
        ExportParticlesToCSV("sph2d", i, particles);
    }
}

TEST_CASE("sph2d", "[pba]")
{
    sph2d();
}