#ifndef SBS_PHYSICS_MECHANICS_MESHLESS_BODY_H
#define SBS_PHYSICS_MECHANICS_MESHLESS_BODY_H

#include <Discregrid/acceleration/bounding_sphere_hierarchy.hpp>
#include <Eigen/Core>
#include <sbs/aliases.h>
#include <sbs/physics/body.h>
#include <sbs/physics/collision/bvh_model.h>
#include <sbs/physics/mechanics/meshless_sph_surface.h>
#include <sbs/physics/topology.h>
#include <vector>

namespace sbs {
namespace common {

struct geometry_t;

} // namespace common
namespace physics {
namespace mechanics {

class meshless_sph_node_t;

class range_searcher_t : public Discregrid::KDTree<Discregrid::BoundingSphere>
{
  public:
    using base_type = Discregrid::KDTree<Discregrid::BoundingSphere>;

    range_searcher_t();
    range_searcher_t(std::vector<meshless_sph_node_t> const* nodes);

    range_searcher_t(range_searcher_t const& other) = default;
    range_searcher_t(range_searcher_t&& other)      = default;

    range_searcher_t& operator=(range_searcher_t const& other) = default;
    range_searcher_t& operator=(range_searcher_t&& other) noexcept = default;

    std::vector<index_type> neighbours_of(index_type const ni) const;
    std::vector<index_type> neighbours_of(Eigen::Vector3d const& p, scalar_type const h) const;

    virtual Eigen::Vector3d entityPosition(unsigned int i) const override final;
    virtual void computeHull(unsigned int b, unsigned int n, Discregrid::BoundingSphere& hull)
        const override final;

  private:
    std::vector<meshless_sph_node_t> const* nodes_;
};

class meshless_sph_body_t : public physics::body_t
{
  public:
    meshless_sph_body_t(
        simulation_t& simulation,
        index_type id,
        common::geometry_t const& geometry,
        scalar_type const h);

    virtual visual_model_type const& visual_model() const override;
    virtual collision_model_type const& collision_model() const override;
    virtual visual_model_type& visual_model() override;
    virtual collision_model_type& collision_model() override;
    virtual void update_visual_model() override;
    virtual void update_collision_model() override;
    virtual void update_physical_model() override;
    virtual void transform(Eigen::Affine3d const& affine) override;

    std::vector<meshless_sph_node_t> const& nodes() const;
    std::vector<meshless_sph_node_t>& nodes();
    meshless_sph_surface_t const& surface_mesh() const;
    meshless_sph_surface_t& surface_mesh();
    collision::point_bvh_model_t const& bvh() const;

    range_searcher_t const& range_searcher() const;
    tetrahedron_set_t const& topology() const;
    tetrahedron_set_t& topology();
    scalar_type h() const;

    void initialize_physical_model();
    void initialize_visual_model();
    void initialize_collision_model();

  private:
    std::vector<meshless_sph_node_t> physical_model_;
    range_searcher_t
        material_space_range_query_; ///< Used for querying neighbours in material space
    tetrahedron_set_t
        volumetric_topology_; ///< Only used for boundary surface extraction, the mechanical
                              ///< representation is still just a set of meshless nodes
    meshless_sph_surface_t visual_model_;
    collision::point_bvh_model_t collision_model_;
    scalar_type h_; ///< Support radius of meshless nodes
};

} // namespace mechanics
} // namespace physics
} // namespace sbs

#endif // SBS_PHYSICS_MECHANICS_MESHLESS_BODY_H