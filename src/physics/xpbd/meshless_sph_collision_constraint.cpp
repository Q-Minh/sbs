#include <iostream>
#include <sbs/physics/mechanics/hybrid_mesh_meshless_sph_node.h>
#include <sbs/physics/mechanics/meshless_sph_body.h>
#include <sbs/physics/mechanics/meshless_sph_surface.h>
#include <sbs/physics/simulation.h>
#include <sbs/physics/xpbd/meshless_sph_collision_constraint.h>

namespace sbs {
namespace physics {
namespace xpbd {

sbs::physics::xpbd::meshless_sph_collision_constraint_t::meshless_sph_collision_constraint_t(
    scalar_type alpha,
    scalar_type beta,
    index_type const bi,
    mechanics::meshless_sph_body_t const* b,
    mechanics::meshless_sph_surface_vertex_t const& vk,
    Eigen::Vector3d const& vi,
    Eigen::Vector3d const& c,
    Eigen::Vector3d const& n)
    : constraint_t(alpha, beta), bi_(bi), b_(b), vk_(vk), vi_(vi), n_(n), c_(c)
{
}

void sbs::physics::xpbd::meshless_sph_collision_constraint_t::project_positions(
    simulation_t& simulation,
    scalar_type dt)
{
    auto& particles = simulation.particles()[bi_];

    auto const& neighbours = vk_.neighbours();
    auto const& Xkjs       = vk_.Xkjs();
    auto const& Wkjs       = vk_.Wkjs();
    auto const& Vjs        = vk_.Vjs();
    auto const sk          = vk_.sk();

    scalar_type const C = (vi_ - c_).dot(n_);

    if (C >= 0.)
    {
        return;
    }

    Eigen::Matrix3d Fi{};

    std::vector<Eigen::Vector3d> gradC{};
    gradC.reserve(neighbours.size());
    scalar_type weighted_sum_of_gradients = 0.;
    for (std::size_t a = 0u; a < neighbours.size(); ++a)
    {
        index_type const j         = neighbours[a];
        scalar_type const& Vj      = Vjs[a];
        scalar_type const& Wkj     = Wkjs[a];
        Eigen::Vector3d const grad = sk * Vj * Wkj * n_;
        particle_t const& p        = particles[j];
        weighted_sum_of_gradients += p.invmass() * grad.squaredNorm();
        gradC.push_back(grad);
    }

    scalar_type const alpha_tilde = alpha_ / (dt * dt);
    scalar_type const delta_lagrange =
        -(C + alpha_tilde * lagrange_) / (weighted_sum_of_gradients + alpha_tilde);

    vi_.setZero();
    for (std::size_t a = 0u; a < neighbours.size(); ++a)
    {
        index_type const j = neighbours[a];
        particle_t& p      = particles[j];
        p.xi() += p.invmass() * gradC[a] * delta_lagrange;

        scalar_type const& Vj      = Vjs[a];
        Eigen::Vector3d const& Xkj = Xkjs[a];
        scalar_type const& Wkj     = Wkjs[a];
        Eigen::Matrix3d const Fj   = b_->nodes()[j].Fi();

        // Update vertex position
        vi_ += sk * Vj * (Fj * Xkj + p.xi()) * Wkj;
    }
}

} // namespace xpbd
} // namespace physics
} // namespace sbs