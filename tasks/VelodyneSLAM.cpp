/* Generated from orogen/lib/orogen/templates/tasks/Task.cpp */

#include "VelodyneSLAM.hpp"

#include <g2o/core/factory.h>
#include <g2o/core/optimization_algorithm_factory.h>
#include <graph_slam/vertex_se3_gicp.hpp>
#include <graph_slam/edge_se3_gicp.hpp>
#include <velodyne_lidar/pointcloudConvertHelper.hpp>
#include <graph_slam/matrix_helper.hpp>

#ifndef MEASURE_TIME
#define MEASURE_TIME(exp, time_in_s) \
{ \
    clock_t _clock_time_ = clock(); \
    exp; \
    time_in_s = (double)(clock() - _clock_time_) / (double)CLOCKS_PER_SEC; \
}
#endif

using namespace graph_slam;

VelodyneSLAM::VelodyneSLAM(std::string const& name)
    : VelodyneSLAMBase(name), map_updated(false)
{
}

VelodyneSLAM::VelodyneSLAM(std::string const& name, RTT::ExecutionEngine* engine)
    : VelodyneSLAMBase(name, engine), map_updated(false)
{
}

VelodyneSLAM::~VelodyneSLAM()
{
}

void VelodyneSLAM::handleLidarData(const base::Time &ts, bool use_simulated_data)
{    
    // get static transformation
    Eigen::Affine3d laser2body;
    if (!_laser2body.get(ts, laser2body))
    {
        RTT::log(RTT::Error) << "skip, have no laser2body transformation sample!" << RTT::endlog();
        new_state = MISSING_TRANSFORMATION;
        return;
    }
    
    // set a odometry covariance if it is nan
    if(is_nan(body2odometry.getCovariance()))
        body2odometry.setCovariance(100 * envire::TransformWithUncertainty::Covariance::Identity());
    
    // reset number of edge candidates handled in the update hook
    try_edges_on_update = 1;

    Eigen::Affine3d odometry_delta = last_odometry_transformation.getTransform().inverse() * body2odometry.getTransform();
    if(odometry_delta.translation().norm() > _vertex_distance.get() || optimizer.vertices().size() == 0)
    {
        try
        {
            // fill envire pointcloud
            std::vector<Eigen::Vector3d> pointcloud;
            if(use_simulated_data)
            {
                // transform pointcloud to body frame
                for(std::vector<base::Vector3d>::const_iterator it = new_simulated_pointcloud_sample.points.begin(); it != new_simulated_pointcloud_sample.points.end(); it++)
                {
                    pointcloud.push_back(laser2body * (*it));
                }
            }
            else
            {
                // filter point cloud
                velodyne_lidar::MultilevelLaserScan filtered_lidar_sample;
                velodyne_lidar::ConvertHelper::filterOutliers(new_lidar_sample, filtered_lidar_sample, _maximum_angle_to_neighbor, _minimum_valid_neighbors);
                
                // add new vertex to graph
                velodyne_lidar::ConvertHelper::convertScanToPointCloud(filtered_lidar_sample, pointcloud, laser2body);
            }
            
            // add new vertex
            if(!optimizer.addVertex(body2odometry, pointcloud))
                throw std::runtime_error("failed to add a new vertex");

            new_vertecies++;
        }
        catch(std::runtime_error e)
        {
            RTT::log(RTT::Error) << "Exception while adding new lidar sample: " << e.what() << RTT::endlog();
            new_state = ADD_VERTEX_FAILED;
        }
        
        last_odometry_transformation = body2odometry;
    }

    // optimization
    unsigned new_edges = optimizer.edges().size() - edge_count;
    if(new_edges >= _run_graph_optimization_counter)
    {
        edge_count = optimizer.edges().size();

        try
        {
            // run graph optimization
            if(optimizer.optimize(2) < 1)
                throw std::runtime_error("edge optimization failed");
            if(_enable_debug)
                writeOptimizerDebugInformation();

            if(new_vertecies >= 5)
            {
                new_vertecies = 0;

                // remove old vertecies
                MEASURE_TIME(optimizer.removeVerticesFromGrid(), debug_information.remove_vertices_time);
                
                // find new edges
                MEASURE_TIME(optimizer.findEdgeCandidates(), debug_information.find_edge_candidates_time);
            }
        }
        catch(std::runtime_error e)
        {
            RTT::log(RTT::Error) << "Exception while optimizing graph: " << e.what() << RTT::endlog();
            new_state = GRAPH_OPTIMIZATION_FAILED;
        }
    }
}

void VelodyneSLAM::lidar_samplesTransformerCallback(const base::Time &ts, const ::velodyne_lidar::MultilevelLaserScan &lidar_sample)
{
    new_state = RUNNING;
    new_lidar_sample = lidar_sample;
    // get dynamic transformation
    if (!_body2odometry.get(ts, body2odometry, true))
    {
        RTT::log(RTT::Error) << "skip, have no body2odometry transformation sample!" << RTT::endlog();
        new_state = MISSING_TRANSFORMATION;
        return;
    }
    handleLidarData(lidar_sample.time, false);
}

bool VelodyneSLAM::generateMap()
{
    bool err = false;
    try
    {
        // run graph optimization
        if(optimizer.optimize(5) < 1)
        {
            err = true;
            RTT::log(RTT::Error) << "optimization failed" << RTT::endlog();
            new_state = GRAPH_OPTIMIZATION_FAILED;
        }
        if(_enable_debug)
            writeOptimizerDebugInformation();
        
        // update envire
        MEASURE_TIME
        (
            if(!optimizer.updateEnvire())
            {
                err = true;
                RTT::log(RTT::Error) << "environment update failed" << RTT::endlog();
                new_state = MAP_GENERATION_FAILED;
            } else {
                map_updated = true;
            },
            debug_information.update_environment_time
        );
    }
    catch(std::runtime_error e)
    {
        RTT::log(RTT::Error) << "Exception while generating MLS map: " << e.what() << RTT::endlog();
        new_state = MAP_GENERATION_FAILED;
    }
    return !err;
}

void VelodyneSLAM::writeOptimizerDebugInformation()
{
    const g2o::BatchStatisticsContainer& stats = optimizer.batchStatistics();
    if(!stats.empty())
    {
        debug_information.graph_num_vertices = stats[0].numVertices;
        debug_information.graph_num_edges = stats[0].numEdges;
        debug_information.graph_chi2_error = stats[stats.size()-1].chi2;
        debug_information.graph_optimization_time = stats[0].timeIteration;
    }
}

/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See VelodyneSLAM.hpp for more detailed
// documentation about them.

bool VelodyneSLAM::configureHook()
{
    if (! VelodyneSLAMBase::configureHook())
        return false;
    
    last_state = PRE_OPERATIONAL;
    new_state = RUNNING;
    new_vertecies = 0;
    edge_count = 0;
    try_edges_on_update = 0;
    last_envire_update.microseconds = 0;
    last_odometry_transformation = envire::TransformWithUncertainty::Identity();
    optimizer.setMLSMapConfiguration(_use_mls, _grid_size_x, _grid_size_y, _cell_resolution_x, _cell_resolution_y, _grid_min_z, _grid_max_z);
    event_filter.reset(new MLSGridEventFilter());
    
    g2o::OptimizableGraph::initMultiThreading();
    
    // set gicp config
    graph_slam::GICPConfiguration gicp_config;
    gicp_config.max_sensor_distance = _max_icp_distance;
    gicp_config.max_fitness_score = _max_icp_fitness_score;
    optimizer.updateGICPConfiguration(gicp_config);

    optimizer.setupMaxVertexGrid(_max_vertices_per_cell, _grid_size_x, _grid_size_y, _vertex_grid_cell_resolution);
    
    return true;
}
bool VelodyneSLAM::startHook()
{
    if (! VelodyneSLAMBase::startHook())
        return false;
    
    return true;
}
void VelodyneSLAM::updateHook()
{
    if( orocos_emitter.use_count() == 0 && _envire_map.connected() )
    {
        // register the binary event dispatcher, 
        // which will write envire data to a port
        boost::shared_ptr<envire::Environment> env = optimizer.getEnvironment();
        orocos_emitter.reset(new envire::OrocosEmitter( _envire_map ));
        orocos_emitter->useContextUpdates( env.get() );
        orocos_emitter->useEventQueue( true );
        orocos_emitter->attach( env.get() );
        if(_use_mls)
            orocos_emitter->setFilter(event_filter.get());
    }

    // enable debug output
    if(_enable_debug)
        optimizer.setComputeBatchStatistics(true);
    
    VelodyneSLAMBase::updateHook();
    
    // write adjusted odometry pose
    base::samples::RigidBodyState odometry_pose;
    if(_odometry_samples.readNewest(odometry_pose) == RTT::NewData) 
    {
        base::samples::RigidBodyState adjusted_odometry_pose = odometry_pose;
        if(optimizer.adjustOdometryPose(odometry_pose, adjusted_odometry_pose))
        {
            adjusted_odometry_pose.sourceFrame = _body_frame.get();
            adjusted_odometry_pose.targetFrame = _world_frame.get();
            //adjusted_odometry_pose.time = base::Time::now(); //TODO Problem/remove?
            _pose_samples.write(adjusted_odometry_pose);
        }
    }
    
    // write envire updates
    if( orocos_emitter.use_count() > 0 )
    {
        if( _envire_map.connected() )
        {
            if( (last_envire_update + base::Time::fromSeconds(_envire_period.value())) < base::Time::now() &&
                    map_updated ) 
            {
                orocos_emitter->flush();
                map_updated = false;
                last_envire_update = base::Time::now();
            }
        }
        else
        {
            orocos_emitter.reset();
        }
    }
    
    // handle simulated data
    if(_simulated_pointcloud.readNewest(new_simulated_pointcloud_sample) == RTT::NewData)
    {
        new_state = RUNNING;
        body2odometry.setTransform(odometry_pose.getTransform());
        body2odometry.setCovariance(combineToPoseCovariance(odometry_pose.cov_position, odometry_pose.cov_orientation));
        if(new_simulated_pointcloud_sample.points.size() > 0)
            handleLidarData(new_simulated_pointcloud_sample.time, true);
    }
    
    // create new edges
    if(try_edges_on_update > 0)
    {
        MEASURE_TIME(optimizer.tryBestEdgeCandidates(1), debug_information.try_edge_candidate_time);
        try_edges_on_update--;
    }

    // write debug information
    if(_enable_debug)
    {
        debug_information.time = base::Time::now();
        _debug_information.write(debug_information);
    }

    // write state if it has changed
    if(last_state != new_state)
    {
        last_state = new_state;
        state(new_state);
    }
}
void VelodyneSLAM::errorHook()
{
    VelodyneSLAMBase::errorHook();
}
void VelodyneSLAM::stopHook()
{
    VelodyneSLAMBase::stopHook();
    
    if(_environment_debug_path.get() != "")
    {
        // write environment
        generateMap();
        optimizer.getEnvironment()->serialize(_environment_debug_path.get());

        // write graph viz
        std::filebuf fb;
        std::string full_path(_environment_debug_path.get());
        full_path += "/graph_viz.dot";
        fb.open (full_path.c_str(),std::ios::out);
        std::ostream os(&fb);
        optimizer.dumpGraphViz(os);
        fb.close();
    }
}
void VelodyneSLAM::cleanupHook()
{
    VelodyneSLAMBase::cleanupHook();
    
    // freeing the graph memory
    optimizer.clear();

    // destroy all the singletons
    g2o::Factory::destroy();
    g2o::OptimizationAlgorithmFactory::destroy();
    g2o::HyperGraphActionLibrary::destroy();
}
