#include "physics/xpbd/solver.h"

#include "physics/collision/brute_force_collision_detector.h"
#include "physics/collision/intersections.h"
#include "physics/xpbd/collision_constraint.h"
#include "physics/xpbd/distance_constraint.h"
#include "physics/xpbd/green_constraint.h"
#include "physics/xpbd/mesh.h"

namespace sbs {
namespace physics {
namespace xpbd {

solver_t::solver_t()
    : physics_bodies_{},
      environment_bodies_{},
      constraints_{},
      collision_constraints_{},
      tetrahedron_to_constraint_map_{},
      dt_{0.0167},
      substeps_{1u},
      iteration_count_{10u}
{
}

solver_t::solver_t(double timestep, std::uint32_t substeps, std::uint32_t iterations)
    : physics_bodies_{},
      environment_bodies_{},
      constraints_{},
      collision_constraints_{},
      tetrahedron_to_constraint_map_{},
      dt_{timestep},
      substeps_{substeps},
      iteration_count_{iterations}
{
}

solver_t::solver_t(std::vector<std::shared_ptr<common::renderable_node_t>> const& bodies)
    : physics_bodies_{},
      environment_bodies_{},
      constraints_{},
      collision_constraints_{},
      tetrahedron_to_constraint_map_{},
      dt_{0.0167},
      substeps_{30u},
      iteration_count_{30u}
{
    setup(bodies);
}

solver_t::solver_t(
    double timestep,
    std::uint32_t substeps,
    std::uint32_t iterations,
    std::vector<std::shared_ptr<common::renderable_node_t>> const& bodies)
    : solver_t{timestep, substeps, iterations}
{
    setup(bodies);
}

void solver_t::setup(std::vector<std::shared_ptr<common::renderable_node_t>> const& bodies)
{
    reset();

    for (auto const& body : bodies)
    {
        std::shared_ptr<xpbd::tetrahedral_mesh_t> const physics_body_ptr =
            std::dynamic_pointer_cast<xpbd::tetrahedral_mesh_t>(body);
        if (static_cast<bool>(physics_body_ptr))
        {
            physics_bodies_.push_back(physics_body_ptr);
            continue;
        }

        std::shared_ptr<common::shared_vertex_surface_mesh_i> const environment_body_ptr =
            std::dynamic_pointer_cast<common::shared_vertex_surface_mesh_i>(body);
        if (static_cast<bool>(environment_body_ptr))
        {
            environment_bodies_.push_back(environment_body_ptr);
            continue;
        }
    }

    for (std::shared_ptr<xpbd::tetrahedral_mesh_t> physics_body : physics_bodies_)
    {
        auto const constraint_type = physics_body->simulation_parameters().constraint_type;

        if (constraint_type == constraint_type_t::green)
        {
            create_green_constraints_for_body(physics_body.get());
        }
        if (constraint_type == constraint_type_t::distance)
        {
            create_distance_constraints_for_body(physics_body.get());
        }
    }

    x0_.resize(physics_bodies_.size());
    lagrange_multipliers_.resize(constraints_.size());
}

void solver_t::reset()
{
    physics_bodies_.clear();
    environment_bodies_.clear();
    constraints_.clear();
    collision_constraints_.clear();
    tetrahedron_to_constraint_map_.clear();
    x0_.clear();
    lagrange_multipliers_.clear();
}

void solver_t::step()
{
    // TODO: cut here
    // ...

    bool can_perform_substepping = (iteration_count_ >= substeps_);
    std::uint32_t const num_iterations =
        can_perform_substepping ?
            static_cast<std::uint32_t>(
                static_cast<double>(iteration_count_) / static_cast<double>(substeps_)) :
            iteration_count_;
    std::uint32_t const num_substeps = can_perform_substepping ? substeps_ : 1u;
    double const dt = can_perform_substepping ? dt_ / static_cast<double>(substeps_) : dt_;

    for (std::uint32_t s = 0u; s < num_substeps; ++s)
    {
        // Hold previous positions x0.
        // Using a vector of vector will result in two levels of indirection before accessing any
        // position. The position vectors will be allocated on the heap at potentially different far
        // locations. We can use a smarter allocation system where these vectors will be stored
        // contiguously.
        std::transform(
            physics_bodies_.begin(),
            physics_bodies_.end(),
            x0_.begin(),
            [](std::shared_ptr<tetrahedral_mesh_t> const& body) {
                std::vector<Eigen::Vector3d> p0{};
                p0.reserve(body->vertices().size());
                std::transform(
                    body->vertices().begin(),
                    body->vertices().end(),
                    std::back_inserter(p0),
                    [](vertex_t const& v) { return v.position(); });
                return p0;
            });

        // explicit integration step
        Eigen::Vector3d const gravity{0., -9.81, 0.};
        for (auto const& body : physics_bodies_)
        {
            std::for_each(
                body->vertices().begin(),
                body->vertices().end(),
                [dt, gravity](vertex_t& vertex) {
                    if (vertex.fixed())
                        return;

                    vertex.force() += gravity;
                    Eigen::Vector3d const acceleration = vertex.force().array() / vertex.mass();
                    vertex.velocity()                  = vertex.velocity() + dt * acceleration;
                    vertex.position() += dt * vertex.velocity();
                });
        }

        // TODO: detect collisions
        // ...
        handle_collisions();

        // constraint projection
        std::size_t const J  = constraints_.size();
        std::size_t const Mc = collision_constraints_.size();
        lagrange_multipliers_.resize(J + Mc);
        std::fill(lagrange_multipliers_.begin(), lagrange_multipliers_.end(), 0.);
        for (std::uint32_t n = 0u; n < num_iterations; ++n)
        {
            for (std::size_t j = 0u; j < J; ++j)
            {
                std::unique_ptr<constraint_t> const& constraint = constraints_[j];
                constraint->project(physics_bodies_, lagrange_multipliers_[j], dt);
            }
            for (std::size_t c = 0u; c < Mc; ++c)
            {
                collision_constraint_t const& constraint = collision_constraints_[c];
                constraint.project(physics_bodies_, lagrange_multipliers_[J + c], dt);
            }
        }
        collision_constraints_.clear();

        // update positions and velocities
        for (std::size_t b = 0u; b < physics_bodies_.size(); ++b)
        {
            auto const& body               = physics_bodies_[b];
            std::size_t const vertex_count = body->vertices().size();
            for (std::size_t vi = 0u; vi < vertex_count; ++vi)
            {
                vertex_t& vertex = body->vertices().at(vi);

                Eigen::Vector3d const x0 = x0_[b][vi];
                Eigen::Vector3d const& x = vertex.position();

                vertex.velocity() = (x - x0) / dt;
                vertex.position() = x;
                vertex.force().setZero();
            }
        }

        // friction or other non-conservative forces here
    }
}

double const& solver_t::timestep() const
{
    return dt_;
}

double& solver_t::timestep()
{
    return dt_;
}

std::uint32_t const& solver_t::iterations() const
{
    return iteration_count_;
}

std::uint32_t& solver_t::iterations()
{
    return iteration_count_;
}

std::uint32_t const& solver_t::substeps() const
{
    return substeps_;
}

std::uint32_t& solver_t::substeps()
{
    return substeps_;
}

double const& solver_t::collision_compliance() const
{
    return collision_alpha_;
}

double& solver_t::collision_compliance()
{
    return collision_alpha_;
}

std::vector<std::shared_ptr<xpbd::tetrahedral_mesh_t>> const& solver_t::simulated_bodies() const
{
    return physics_bodies_;
}

void solver_t::create_green_constraints_for_body(xpbd::tetrahedral_mesh_t* body)
{
    simulation_parameters_t const& params = body->simulation_parameters();
    std::size_t const tetrahedron_count   = body->tetrahedra().size();
    for (std::size_t ti = 0u; ti < tetrahedron_count; ++ti)
    {
        tetrahedron_t const& tetrahedron = body->tetrahedra().at(ti);
        auto constraint                  = std::make_unique<green_constraint_t>(
            params.alpha,
            body,
            static_cast<physics::index_type>(ti),
            params.young_modulus,
            params.poisson_ratio);

        constraint_map_key_type const key   = std::make_pair(body, static_cast<std::uint32_t>(ti));
        tetrahedron_to_constraint_map_[key] = static_cast<std::uint32_t>(constraints_.size());
        constraints_.push_back(std::move(constraint));
    }
}

void solver_t::create_distance_constraints_for_body(xpbd::tetrahedral_mesh_t* body)
{
    simulation_parameters_t const& params = body->simulation_parameters();
    for (edge_t const& edge : body->edges())
    {
        auto constraint = std::make_unique<distance_constraint_t>(
            1. / params.hooke_coefficient,
            std::make_pair(body, edge.v1()),
            std::make_pair(body, edge.v2()));

        constraints_.push_back(std::move(constraint));
    }
}

void solver_t::handle_collisions()
{
    brute_force_collision_detector_t<xpbd::tetrahedral_mesh_t> collision_detection_system{};

    for (auto const& env_body : environment_bodies_)
    {
        collision_detection_system.add_environment_body(env_body.get());
    }

    std::vector<bool> is_vertex_constrained_by_collision{};
    for (auto const& phys_body : physics_bodies_)
    {
        is_vertex_constrained_by_collision.clear();
        is_vertex_constrained_by_collision.resize(phys_body->vertices().size(), false);

        auto const intersection_pairs = collision_detection_system.intersect(phys_body.get());
        for (auto const& [ti, triangle] : intersection_pairs)
        {
            auto const& tetrahedron = phys_body->tetrahedra().at(ti);
            std::array<index_type, 4u> const v{
                tetrahedron.v1(),
                tetrahedron.v2(),
                tetrahedron.v3(),
                tetrahedron.v4()};

            auto const n = triangle.normal();
            for (std::size_t j = 0u; j < v.size(); ++j)
            {
                // check if vertex is penetrating the triangle
                index_type const vi               = v[j];
                auto const& vertex                = phys_body->vertices().at(vi);
                auto const& p                     = vertex.position();
                common::vector3d_t const d        = p - triangle.a();
                bool const is_vertex_over_surface = d.dot(n) >= 0.;

                if (is_vertex_over_surface)
                    continue;

                if (is_vertex_constrained_by_collision[vi])
                    continue;

                collision_constraints_.push_back(
                    collision_constraint_t{collision_alpha_, phys_body.get(), vi, triangle, n});

                is_vertex_constrained_by_collision[vi] = true;
            }
        }
    }
}

} // namespace xpbd
} // namespace physics
} // namespace sbs