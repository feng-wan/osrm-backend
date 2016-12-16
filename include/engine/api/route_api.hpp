#ifndef ENGINE_API_ROUTE_HPP
#define ENGINE_API_ROUTE_HPP

#include "engine/api/base_api.hpp"
#include "engine/api/json_factory.hpp"
#include "engine/api/route_parameters.hpp"

#include "engine/datafacade/datafacade_base.hpp"

#include "engine/guidance/assemble_geometry.hpp"
#include "engine/guidance/assemble_leg.hpp"
#include "engine/guidance/assemble_overview.hpp"
#include "engine/guidance/assemble_route.hpp"
#include "engine/guidance/assemble_steps.hpp"
#include "engine/guidance/lane_processing.hpp"
#include "engine/guidance/post_processing.hpp"

#include "engine/internal_route_result.hpp"

#include "util/coordinate.hpp"
#include "util/integer_range.hpp"

#include <iterator>
#include <vector>

namespace osrm
{
namespace engine
{
namespace api
{

class RouteAPI : public BaseAPI
{
  public:
    RouteAPI(const datafacade::BaseDataFacade &facade_, const RouteParameters &parameters_)
        : BaseAPI(facade_, parameters_), parameters(parameters_)
    {
    }

    void MakeResponse(const InternalRouteResult &raw_route, util::json::Object &response) const
    {
        if (parameters.xad_pois)
        {
            util::json::Array pois;
            MakeXadPois(raw_route.unpacked_path_segments, pois);
            response.values["xad_pois"] = std::move(pois);
        }
        else
        {
            auto number_of_routes = raw_route.has_alternative() ? 2UL : 1UL;
            util::json::Array routes;
            routes.values.resize(number_of_routes);
            routes.values[0] = MakeRoute(raw_route.segment_end_coordinates,
                                         raw_route.unpacked_path_segments,
                                         raw_route.source_traversed_in_reverse,
                                         raw_route.target_traversed_in_reverse);
            if (raw_route.has_alternative())
            {
                std::vector<std::vector<PathData>> wrapped_leg(1);
                wrapped_leg.front() = std::move(raw_route.unpacked_alternative);
                routes.values[1] = MakeRoute(raw_route.segment_end_coordinates,
                                             wrapped_leg,
                                             raw_route.alt_source_traversed_in_reverse,
                                             raw_route.alt_target_traversed_in_reverse);
            }
            response.values["waypoints"] = BaseAPI::MakeWaypoints(raw_route.segment_end_coordinates);
            response.values["routes"] = std::move(routes);
        }
        response.values["code"] = "Ok";
    }

    // FIXME gcc 4.8 doesn't support for lambdas to call protected member functions
    //  protected:
    template <typename ForwardIter>
    util::json::Value MakeGeometry(ForwardIter begin, ForwardIter end) const
    {
        if (parameters.geometries == RouteParameters::GeometriesType::Polyline)
        {
            return json::makePolyline(begin, end);
        }

        BOOST_ASSERT(parameters.geometries == RouteParameters::GeometriesType::GeoJSON);
        return json::makeGeoJSONGeometry(begin, end);
    }
    
    // calculat xad pois along the route
    void MakeXadPois( const std::vector<std::vector<PathData>> &unpacked_path_segments, util::json::Array& json_pois) const
    {
        std::size_t number_of_path = unpacked_path_segments.size();
        BOOST_ASSERT(number_of_path>0);
        const auto &path_data = unpacked_path_segments[0];
        BOOST_ASSERT(path_data.size()>=2);  // at least 2 nodes
        // nodes contain all paths' nodes; The initial value of 0 serves as sentinal
        std::vector<NodeID> nodes;
        for (auto it = path_data.begin(); it!= path_data.end(); ++it)
            nodes.push_back(it->turn_via_node);
        BOOST_ASSERT(nodes.size()>=2);
        for (auto idx : util::irange<std::size_t>(1UL, number_of_path))
        {
            const auto &path_data = unpacked_path_segments[idx];
            BOOST_ASSERT(path_data.size()>=2);  // at least 2 nodes
            // skip the first 2 nodes, because they are duplicated, like example below:
            // http://10.10.10.88:5000/route/v1/driving/-122.10785,37.43467;-122.108751,37.43544;-122.10898,37.43563?annotations=true
            for (auto it = path_data.begin()+2; it!= path_data.end(); ++it)
                nodes.push_back(it->turn_via_node);
        }
        for (auto it = nodes.begin(); it+1!= nodes.end(); ++it)
        {
            auto pois_data = facade.GetXadPoisOfNode(*it);
            if (pois_data)
            {
                BOOST_ASSERT(!pois_data->empty());
                for (auto it_poi = pois_data->begin(); it_poi!= pois_data->end(); ++it_poi)
                {
                    if (it_poi->CanIgnoreNode2())
                    {
                        json_pois.values.push_back(it_poi->GetPoiId());
                    }
                    else
                    {
                        std::uint64_t path_node2 = static_cast<std::uint64_t>( facade.GetOSMNodeIDOfNode(*(it+1)));
                        if (path_node2 == it_poi->GetNode2())
                        {
                            json_pois.values.push_back(it_poi->GetPoiId());
                        }
                    }
                }
            }
        }
    }
    util::json::Object MakeRoute(const std::vector<PhantomNodes> &segment_end_coordinates,
                                 const std::vector<std::vector<PathData>> &unpacked_path_segments,
                                 const std::vector<bool> &source_traversed_in_reverse,
                                 const std::vector<bool> &target_traversed_in_reverse) const
    {
        std::vector<guidance::RouteLeg> legs;
        std::vector<guidance::LegGeometry> leg_geometries;
        auto number_of_legs = segment_end_coordinates.size();
        legs.reserve(number_of_legs);
        leg_geometries.reserve(number_of_legs);

        for (auto idx : util::irange<std::size_t>(0UL, number_of_legs))
        {
            const auto &phantoms = segment_end_coordinates[idx];
            const auto &path_data = unpacked_path_segments[idx];

            const bool reversed_source = source_traversed_in_reverse[idx];
            const bool reversed_target = target_traversed_in_reverse[idx];

            auto leg_geometry = guidance::assembleGeometry(
                BaseAPI::facade, path_data, phantoms.source_phantom, phantoms.target_phantom);
            auto leg = guidance::assembleLeg(facade,
                                             path_data,
                                             leg_geometry,
                                             phantoms.source_phantom,
                                             phantoms.target_phantom,
                                             reversed_target,
                                             parameters.steps);

            if (parameters.steps)
            {
                auto steps = guidance::assembleSteps(BaseAPI::facade,
                                                     path_data,
                                                     leg_geometry,
                                                     phantoms.source_phantom,
                                                     phantoms.target_phantom,
                                                     reversed_source,
                                                     reversed_target);

                /* Perform step-based post-processing.
                 *
                 * Using post-processing on basis of route-steps for a single leg at a time
                 * comes at the cost that we cannot count the correct exit for roundabouts.
                 * We can only emit the exit nr/intersections up to/starting at a part of the leg.
                 * If a roundabout is not terminated in a leg, we will end up with a
                 *enter-roundabout
                 * and exit-roundabout-nr where the exit nr is out of sync with the previous enter.
                 *
                 *         | S |
                 *         *   *
                 *  ----*        * ----
                 *                  T
                 *  ----*        * ----
                 *       V *   *
                 *         |   |
                 *         |   |
                 *
                 * Coming from S via V to T, we end up with the legs S->V and V->T. V-T will say to
                 *take
                 * the second exit, even though counting from S it would be the third.
                 * For S, we only emit `roundabout` without an exit number, showing that we enter a
                 *roundabout
                 * to find a via point.
                 * The same exit will be emitted, though, if we should start routing at S, making
                 * the overall response consistent.
                 */

                guidance::trimShortSegments(steps, leg_geometry);
                leg.steps = guidance::postProcess(std::move(steps));
                leg.steps = guidance::collapseTurns(std::move(leg.steps));
                leg.steps = guidance::buildIntersections(std::move(leg.steps));
                leg.steps = guidance::assignRelativeLocations(std::move(leg.steps),
                                                              leg_geometry,
                                                              phantoms.source_phantom,
                                                              phantoms.target_phantom);
                leg.steps = guidance::removeLanesFromRoundabouts(std::move(leg.steps));
                leg.steps = guidance::anticipateLaneChange(std::move(leg.steps));
                leg.steps = guidance::collapseUseLane(std::move(leg.steps));
                leg_geometry = guidance::resyncGeometry(std::move(leg_geometry), leg.steps);
            }

            leg_geometries.push_back(std::move(leg_geometry));
            legs.push_back(std::move(leg));
        }

        auto route = guidance::assembleRoute(legs);
        boost::optional<util::json::Value> json_overview;
        if (parameters.overview != RouteParameters::OverviewType::False)
        {
            const auto use_simplification =
                parameters.overview == RouteParameters::OverviewType::Simplified;
            BOOST_ASSERT(use_simplification ||
                         parameters.overview == RouteParameters::OverviewType::Full);

            auto overview = guidance::assembleOverview(leg_geometries, use_simplification);
            json_overview = MakeGeometry(overview.begin(), overview.end());
        }

        std::vector<util::json::Value> step_geometries;
        for (const auto idx : util::irange<std::size_t>(0UL, legs.size()))
        {
            auto &leg_geometry = leg_geometries[idx];

            step_geometries.reserve(step_geometries.size() + legs[idx].steps.size());

            std::transform(
                legs[idx].steps.begin(),
                legs[idx].steps.end(),
                std::back_inserter(step_geometries),
                [this, &leg_geometry](const guidance::RouteStep &step) {
                    if (parameters.geometries == RouteParameters::GeometriesType::Polyline)
                    {
                        return static_cast<util::json::Value>(
                            json::makePolyline(leg_geometry.locations.begin() + step.geometry_begin,
                                               leg_geometry.locations.begin() + step.geometry_end));
                    }
                    BOOST_ASSERT(parameters.geometries == RouteParameters::GeometriesType::GeoJSON);
                    return static_cast<util::json::Value>(json::makeGeoJSONGeometry(
                        leg_geometry.locations.begin() + step.geometry_begin,
                        leg_geometry.locations.begin() + step.geometry_end));
                });
        }

        std::vector<util::json::Object> annotations;
        if (parameters.annotations)
        {
            for (const auto idx : util::irange<std::size_t>(0UL, leg_geometries.size()))
            {
                util::json::Array durations;
                util::json::Array distances;
                util::json::Array nodes;
                util::json::Array datasources;
                auto &leg_geometry = leg_geometries[idx];

                durations.values.reserve(leg_geometry.annotations.size());
                distances.values.reserve(leg_geometry.annotations.size());
                nodes.values.reserve(leg_geometry.osm_node_ids.size());
                datasources.values.reserve(leg_geometry.annotations.size());

                std::for_each(leg_geometry.annotations.begin(),
                              leg_geometry.annotations.end(),
                              [this, &durations, &distances, &datasources](
                                  const guidance::LegGeometry::Annotation &step) {
                                  durations.values.push_back(step.duration);
                                  distances.values.push_back(step.distance);
                                  datasources.values.push_back(step.datasource);
                              });
                std::for_each(leg_geometry.osm_node_ids.begin(),
                              leg_geometry.osm_node_ids.end(),
                              [this, &nodes](const OSMNodeID &node_id) {
                                  nodes.values.push_back(static_cast<std::uint64_t>(node_id));
                              });
                util::json::Object annotation;
                annotation.values["distance"] = std::move(distances);
                annotation.values["duration"] = std::move(durations);
                annotation.values["nodes"] = std::move(nodes);
                annotation.values["datasources"] = std::move(datasources);
                annotations.push_back(std::move(annotation));
            }
        }

        auto result = json::makeRoute(route,
                                      json::makeRouteLegs(std::move(legs),
                                                          std::move(step_geometries),
                                                          std::move(annotations)),
                                      std::move(json_overview));

        return result;
    }

    const RouteParameters &parameters;
};

} // ns api
} // ns engine
} // ns osrm

#endif
