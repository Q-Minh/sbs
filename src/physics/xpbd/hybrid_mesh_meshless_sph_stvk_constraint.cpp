#include <iterator>
#include <sbs/physics/mechanics/hybrid_mesh_meshless_sph_body.h>
#include <sbs/physics/mechanics/hybrid_mesh_meshless_sph_node.h>
#include <sbs/physics/particle.h>
#include <sbs/physics/simulation.h>
#include <sbs/physics/xpbd/hybrid_mesh_meshless_sph_stvk_constraint.h>

namespace sbs {
namespace physics {
namespace xpbd {

hybrid_mesh_meshless_sph_constraint_t::hybrid_mesh_meshless_sph_constraint_t(
    scalar_type const alpha,
    scalar_type const beta,
    simulation_t const& simulation,
    index_type bi,
    index_type ni,
    scalar_type young_modulus,
    scalar_type poisson_ratio,
    mechanics::hybrid_mesh_meshless_sph_node_t& node)
    : constraint_t(alpha, beta), node_(node), bi_(bi), ni_(ni), mu_(), lambda_()
{
    mu_     = (young_modulus) / (2. * (1 + poisson_ratio));
    lambda_ = (young_modulus * poisson_ratio) / ((1 + poisson_ratio) * (1 - 2 * poisson_ratio));
}

void hybrid_mesh_meshless_sph_constraint_t::project_positions(
    simulation_t& simulation,
    scalar_type dt)
{
    auto& particles                                        = simulation.particles()[bi_];
    mechanics::hybrid_mesh_meshless_sph_body_t const& body = node_.body();
    std::size_t const mesh_particle_offset                 = body.get_mesh_particles_index_offset();
    std::size_t const meshless_particle_offset = body.get_meshless_particles_index_offset();

    std::vector<index_type> const& neighbours = node_.neighbours();
    functions::poly6_kernel_t const& kernel   = node_.kernel();
    Eigen::Matrix3d const Fi                  = deformation_gradient(simulation);

    node_.Fi()                 = Fi;
    Eigen::Matrix3d const Ei   = green_strain(Fi);
    auto const& [Psi, dPsidFi] = strain_energy_and_stress(Fi, Ei);

    scalar_type const C = node_.Vi() * Psi;

    // d (Fi) / d (xk) yields a 3x3x3 tensor, but each d (Fi) / d (xk)^(l), for l=1,2,3,
    // l representing the component of xk, d (Fi) / d (xk)^(l) is a 3x3 matrix
    std::vector<Eigen::Vector3d> const gradC = dCdxk(dPsidFi);

    scalar_type weighted_sum_of_gradients{0.};
    scalar_type gradC_dot_displacement{0.};
    for (std::size_t a = 0u; a < neighbours.size(); ++a)
    {
        index_type const j   = neighbours[a];
        particle_t const& pj = particles[meshless_particle_offset + j];
        weighted_sum_of_gradients += pj.invmass() * gradC[a].squaredNorm();
        gradC_dot_displacement += gradC[a].dot(pj.xi() - pj.xn());
    }

    std::array<std::optional<Eigen::Vector3d>, 4u> const gradC_wrt_mesh_indices = dCdvi(dPsidFi);
    for (std::uint8_t i = 0u; i < 4u; ++i)
    {
        if (gradC_wrt_mesh_indices[i].has_value())
        {
            tetrahedron_t const& t = body.topology().tetrahedron(node_.ti());
            index_type const vi    = t.vertex_indices()[i];
            particle_t const& pi   = particles[vi];
            weighted_sum_of_gradients += pi.invmass() * gradC_wrt_mesh_indices[i]->squaredNorm();
            gradC_dot_displacement += gradC_wrt_mesh_indices[i]->dot(pi.xi() - pi.xn());
        }
    }

    scalar_type constexpr epsilon = 1e-20;
    if (weighted_sum_of_gradients < epsilon)
        return;

    scalar_type const dt2         = dt * dt;
    scalar_type const alpha_tilde = alpha() / dt2;
    scalar_type const beta_tilde  = beta() * dt2;
    scalar_type const gamma       = alpha_tilde * beta_tilde / dt;

    scalar_type const delta_lagrange_num =
        -(C + alpha_tilde * lagrange_) - gamma * gradC_dot_displacement;
    scalar_type const delta_lagrange_den = (1. + gamma) * (weighted_sum_of_gradients) + alpha_tilde;
    scalar_type const delta_lagrange     = delta_lagrange_num / delta_lagrange_den;

    lagrange_ += delta_lagrange;

    // Update meshless particles
    for (std::size_t a = 0u; a < neighbours.size(); ++a)
    {
        index_type const j = neighbours[a];
        particle_t& pj     = particles[meshless_particle_offset + j];
        pj.xi() += pj.invmass() * gradC[a] * delta_lagrange;
    }
    // Update mesh nodes
    for (std::uint8_t i = 0u; i < 4u; ++i)
    {
        if (gradC_wrt_mesh_indices[i].has_value())
        {
            tetrahedron_t const& t = body.topology().tetrahedron(node_.ti());
            index_type const vi    = t.vertex_indices()[i];
            particle_t& pi         = particles[mesh_particle_offset + vi];
            pi.xi() += pi.invmass() * gradC_wrt_mesh_indices[i].value() * delta_lagrange;
        }
    }
}

Eigen::Matrix3d
hybrid_mesh_meshless_sph_constraint_t::deformation_gradient(simulation_t& simulation) const
{
    auto& particles                           = simulation.particles()[bi_];
    std::vector<index_type> const& neighbours = node_.neighbours();
    functions::poly6_kernel_t const& kernel   = node_.kernel();

    mechanics::hybrid_mesh_meshless_sph_body_t const& body = node_.body();
    std::size_t const mesh_particle_offset                 = body.get_mesh_particles_index_offset();
    std::size_t const meshless_particle_offset = body.get_meshless_particles_index_offset();

    Eigen::Matrix3d Fi{};
    Fi.setZero();
    for (std::size_t k = 0u; k < neighbours.size(); ++k)
    {
        index_type const j             = neighbours[k];
        scalar_type const Vj           = node_.Vjs()[k];
        Eigen::Matrix3d const& Li      = node_.Li();
        Eigen::Vector3d const& gradWij = node_.gradWij()[k];
        particle_t const& pi           = particles[meshless_particle_offset + ni_];
        particle_t const& pj           = particles[meshless_particle_offset + j];
        Eigen::Vector3d const xji      = pj.xi() - pi.xi();
        Eigen::Matrix3d const Fij      = Vj * xji * (Li * gradWij).transpose();
        Fi += Fij;
    }
    if (node_.is_mixed_particle())
    {
        //    index_type const ti    = node_.ti();
        //    tetrahedron_t const& t = body.topology().tetrahedron(ti);
        //    for (std::uint8_t i = 0u; i < 4u; ++i)
        //    {
        //        index_type const vi = t.vertex_indices()[i];
        //        // boundary mesh vertices have no shape function, so
        //        // they do not contribute to the interpolation scheme
        //        if (body.is_boundary_mesh_vertex(vi))
        //            continue;

        //        Eigen::Vector3d const& grad_phi_i = body.grad_phi_i(ti, i);
        //        particle_t const& p               = particles[mesh_particle_offset + vi];
        //        Eigen::Matrix3d const j           = p.xi() * grad_phi_i.transpose();
        //        Fi += j;
        //    }
    }

    return Fi;
}

Eigen::Matrix3d hybrid_mesh_meshless_sph_constraint_t::green_strain(Eigen::Matrix3d const& Fi) const
{
    Eigen::Matrix3d const Ei =
        scalar_type{0.5} * (Fi.transpose() * Fi - Eigen::Matrix3d::Identity());
    return Ei;
}

std::pair<scalar_type, Eigen::Matrix3d>
hybrid_mesh_meshless_sph_constraint_t::strain_energy_and_stress(
    Eigen::Matrix3d const& Fi,
    Eigen::Matrix3d const& Ei) const
{
    scalar_type const Eitrace = Ei.trace();
    scalar_type const Psi =
        mu_ * (Ei.array() * Ei.array()).sum() + 0.5 * lambda_ * (Eitrace * Eitrace);
    scalar_type const C = node_.Vi() * Psi;

    // d (Psi) / d (Fi) yields a 3x3 matrix
    Eigen::Matrix3d const dPsidFi =
        Fi * (2. * mu_ * Ei + lambda_ * Eitrace * Eigen::Matrix3d::Identity());

    return std::make_pair(Psi, dPsidFi);
}

scalar_type const hybrid_mesh_meshless_sph_constraint_t::C(scalar_type const Psi) const
{
    return node_.Vi() * Psi;
}

std::vector<Eigen::Vector3d>
hybrid_mesh_meshless_sph_constraint_t::dCdxk(Eigen::Matrix3d const& dPsidFi) const
{
    std::vector<index_type> const& neighbours = node_.neighbours();

    std::vector<Eigen::Vector3d> gradC{};
    gradC.resize(neighbours.size(), Eigen::Vector3d{0., 0., 0.});
    index_type const i_idx = static_cast<index_type>(
        std::distance(neighbours.begin(), std::find(neighbours.begin(), neighbours.end(), ni_)));
    for (std::size_t a = 0u; a < neighbours.size(); ++a)
    {
        index_type const k = neighbours[a];

        if (k == ni_)
            continue;

        Eigen::Matrix3d const& Li = node_.Li();

        index_type const j              = k;
        scalar_type const Vj            = node_.Vjs()[a];
        Eigen::Vector3d const& gradWij  = node_.gradWij()[a];
        Eigen::Vector3d const LiGradWij = Li * gradWij;

        Eigen::Vector3d const& dFidxj_1 = Vj * LiGradWij;
        Eigen::Vector3d const& dFidxj_2 = dFidxj_1;
        Eigen::Vector3d const& dFidxj_3 = dFidxj_1;

        Eigen::Vector3d gradPsi{0., 0., 0.};
        gradPsi(0u) = node_.Vi() * dPsidFi.row(0u).dot(dFidxj_1);
        gradPsi(1u) = node_.Vi() * dPsidFi.row(1u).dot(dFidxj_2);
        gradPsi(2u) = node_.Vi() * dPsidFi.row(2u).dot(dFidxj_3);

        gradC[a] += gradPsi;
        gradC[i_idx] -= gradPsi;
    }

    return gradC;
}

std::array<std::optional<Eigen::Vector3d>, 4u>
hybrid_mesh_meshless_sph_constraint_t::dCdvi(Eigen::Matrix3d const& dPsidFi) const
{
    std::array<std::optional<Eigen::Vector3d>, 4u> grad{};
    if (node_.is_mixed_particle())
    {
        mechanics::hybrid_mesh_meshless_sph_body_t const& body = node_.body();
        index_type const ti                                    = node_.ti();
        tetrahedron_t const& t                                 = body.topology().tetrahedron(ti);
        for (std::uint8_t i = 0u; i < 4u; ++i)
        {
            index_type const vi = t.vertex_indices()[i];
            // boundary mesh vertices have no shape function, so
            // they do not have a term in the gradient operator expression
            if (body.is_boundary_mesh_vertex(vi))
                continue;

            Eigen::Vector3d const& grad_phi_i = body.grad_phi_i(ti, i);
            Eigen::Vector3d const& dFidxk_1   = grad_phi_i;
            Eigen::Vector3d const& dFidxk_2   = dFidxk_1;
            Eigen::Vector3d const& dFidxk_3   = dFidxk_1;

            Eigen::Vector3d gradPsi{0., 0., 0.};
            gradPsi(0u) = node_.Vi() * dPsidFi.row(0u).dot(dFidxk_1);
            gradPsi(1u) = node_.Vi() * dPsidFi.row(1u).dot(dFidxk_2);
            gradPsi(2u) = node_.Vi() * dPsidFi.row(2u).dot(dFidxk_3);

            grad[i] = gradPsi;
        }
    }
    return grad;
}

} // namespace xpbd
} // namespace physics
} // namespace sbs